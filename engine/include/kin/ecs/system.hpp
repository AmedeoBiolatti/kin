#pragma once

#include <kin/core/jobs.hpp>
#include <kin/core/types.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace kin {

enum class SystemPhase {
    Input,
    PreUpdate,
    Update,
    PostUpdate,
    PrePhysics,
    Physics,
    PostPhysics,
    Animation,
    Audio,
    PreRender,
    Render,
    PostRender,
    Editor,
};

// Number of SystemPhase values. Must match the enum above; used to size the
// per-phase arrays in this header and in system.cpp.
inline constexpr std::size_t system_phase_count = 13;

enum class SystemKind {
    Native,
    DataNative,
    Script,
};

enum class SystemOwnerScope {
    Project,
    Scene,
    Editor,
};

enum class SystemAccessMode {
    Read,
    Write,
};

enum class SystemExecutionMode {
    SerialGraph,
    ParallelBatches,
};

enum class SystemExecutionDecision {
    Serial,
    ParallelAcrossSystems,
    ParallelWithinNativeSystem,
};

enum class SystemExecutionPolicy {
    MainThreadOnly,
    ParallelEligible,
};

enum class SystemStructuralMutationPolicy {
    None,
    CommandBufferOnly,
    RawWorldAllowed,
};

struct SystemRelationAccess {
    std::string relation;
    SystemAccessMode mode = SystemAccessMode::Read;
};

inline SystemRelationAccess read_relation(std::string relation) {
    return {
        .relation = std::move(relation),
        .mode = SystemAccessMode::Read,
    };
}

inline SystemRelationAccess write_relation(std::string relation) {
    return {
        .relation = std::move(relation),
        .mode = SystemAccessMode::Write,
    };
}

struct SystemCommandStats {
    u64 queued = 0;
    u64 flushed = 0;
    u64 discarded = 0;
    u64 failed = 0;
};

// Parallel/serial decisions must stay pure functions of deterministic inputs
// (entity counts, system counts). A duration-based gate was removed here: it
// flipped with timing noise, and serial vs staged-parallel runs merge deferred
// ops in different orders, silently breaking run-vs-run determinism.
struct SystemExecutionTuning {
    i32 min_parallel_systems = 2;
    i32 min_native_entities = 1024;
    i32 max_workers = 0;
};

struct SystemParallelDiagnostic {
    SystemId system;
    std::string code;
    std::string message;
};

struct EcsDeferredEntity {
    u32 index = 0;
};

class EcsCommandBuffer {
public:
    EcsDeferredEntity create_entity(std::string name = {});
    void destroy(EcsEntity entity);
    void destroy(EcsId entity);
    void destroy(EcsDeferredEntity entity);

    void add(EcsEntity entity, std::string component);
    void add(EcsId entity, std::string component);
    void add(EcsDeferredEntity entity, std::string component);
    void remove(EcsEntity entity, std::string component);
    void remove(EcsId entity, std::string component);
    void remove(EcsDeferredEntity entity, std::string component);
    void patch_field(EcsEntity entity, std::string component, std::string field, ComponentFieldValue value);
    void patch_field(EcsId entity, std::string component, std::string field, ComponentFieldValue value);
    void patch_field(EcsDeferredEntity entity, std::string component, std::string field, ComponentFieldValue value);

    template <typename T>
    void add(EcsEntity entity) {
        add_command({TargetKind::ExistingId, entity.id(), 0}, [](EcsEntity target) {
            target.add<T>();
        });
    }

    template <typename T>
    void add(EcsId entity) {
        add_command({TargetKind::ExistingId, entity, 0}, [](EcsEntity target) {
            target.add<T>();
        });
    }

    template <typename T>
    void add(EcsDeferredEntity entity) {
        add_command({TargetKind::Deferred, 0, entity.index}, [](EcsEntity target) {
            target.add<T>();
        });
    }

    template <typename T>
    void remove(EcsEntity entity) {
        add_command({TargetKind::ExistingId, entity.id(), 0}, [](EcsEntity target) {
            target.remove<T>();
        });
    }

    template <typename T>
    void remove(EcsId entity) {
        add_command({TargetKind::ExistingId, entity, 0}, [](EcsEntity target) {
            target.remove<T>();
        });
    }

    template <typename T>
    void remove(EcsDeferredEntity entity) {
        add_command({TargetKind::Deferred, 0, entity.index}, [](EcsEntity target) {
            target.remove<T>();
        });
    }

    template <typename T>
    void set(EcsEntity entity, T value) {
        add_command({TargetKind::ExistingId, entity.id(), 0}, [value = std::move(value)](EcsEntity target) mutable {
            target.set<T>(std::move(value));
        });
    }

    template <typename T>
    void set(EcsId entity, T value) {
        add_command({TargetKind::ExistingId, entity, 0}, [value = std::move(value)](EcsEntity target) mutable {
            target.set<T>(std::move(value));
        });
    }

    template <typename T>
    void set(EcsDeferredEntity entity, T value) {
        add_command({TargetKind::Deferred, 0, entity.index}, [value = std::move(value)](EcsEntity target) mutable {
            target.set<T>(std::move(value));
        });
    }

    template <typename T>
    void modified(EcsEntity entity) {
        add_command({TargetKind::ExistingId, entity.id(), 0}, [](EcsEntity target) {
            target.modified<T>();
        });
    }

    template <typename T>
    void modified(EcsId entity) {
        add_command({TargetKind::ExistingId, entity, 0}, [](EcsEntity target) {
            target.modified<T>();
        });
    }

    template <typename T>
    void modified(EcsDeferredEntity entity) {
        add_command({TargetKind::Deferred, 0, entity.index}, [](EcsEntity target) {
            target.modified<T>();
        });
    }

    bool empty() const { return _commands.empty(); }
    std::size_t size() const { return _commands.size(); }
    void clear();
    bool flush(EcsWorld& world, std::string& error);

private:
    enum class TargetKind {
        ExistingId,
        Deferred,
    };

    struct Target {
        TargetKind kind = TargetKind::ExistingId;
        EcsId entity = 0;
        u32 deferred_index = 0;
    };

    using Command = std::function<bool(EcsWorld&, std::vector<EcsEntity>&, std::string&)>;

    static EcsEntity entity_from_id(EcsWorld& world, EcsId id);
    static bool resolve_target(EcsWorld& world, const std::vector<EcsEntity>& created, Target target, EcsEntity& entity, std::string& error);
    void add_command(Target target, std::function<void(EcsEntity)> apply);
    void add_world_command(Target target, std::function<bool(EcsWorld&, EcsEntity, std::string&)> apply);

    std::vector<Command> _commands;
    u32 _next_deferred_entity = 0;
};

struct SystemContext {
    EcsWorld* world = nullptr;
    EcsCommandBuffer* commands = nullptr;
    std::string_view system_id;
    SystemPhase phase = SystemPhase::Update;
    f32 dt = 0.0f;
    bool running_parallel = false;
};

struct SystemRunStats {
    u64 runs = 0;
    i32 matched_entities = 0;
    f64 last_duration_ms = 0.0;
    f64 average_duration_ms = 0.0;
    f64 max_duration_ms = 0.0;
};

// NOTE: SystemDescriptor, NativeSystemDescriptor<>, and SystemSnapshot share an
// authoring field set (id, name, phase, order, interval_seconds, rate, reads,
// writes, relations, before, after, enabled, execution_policy,
// structural_mutation_policy, mutation_notes, source_file, owner_scope). They are
// constructed with designated initializers (here and in game code), which rules
// out sharing those fields via a base class or nested member. Keep the three in
// sync when adding or renaming an authoring attribute.
struct SystemDescriptor {
    SystemId id;
    std::string name;
    SystemKind kind = SystemKind::Native;
    SystemPhase phase = SystemPhase::Update;
    i32 order = 0;
    f32 interval_seconds = 0.0f;
    i32 rate = 1;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<SystemRelationAccess> relations;
    std::vector<SystemId> before;
    std::vector<SystemId> after;
    bool enabled = true;
    SystemExecutionPolicy execution_policy = SystemExecutionPolicy::MainThreadOnly;
    SystemStructuralMutationPolicy structural_mutation_policy = SystemStructuralMutationPolicy::None;
    std::string mutation_notes;
    std::string source_file;
    SystemOwnerScope owner_scope = SystemOwnerScope::Project;
};

template <typename... Components>
struct NativeSystemDescriptor {
    SystemId id;
    std::string name;
    SystemPhase phase = SystemPhase::Update;
    i32 order = 0;
    f32 interval_seconds = 0.0f;
    i32 rate = 1;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<SystemRelationAccess> relations;
    std::vector<SystemId> before;
    std::vector<SystemId> after;
    bool enabled = true;
    SystemExecutionPolicy execution_policy = SystemExecutionPolicy::MainThreadOnly;
    SystemStructuralMutationPolicy structural_mutation_policy = SystemStructuralMutationPolicy::None;
    std::string mutation_notes;
    std::string source_file;
    SystemOwnerScope owner_scope = SystemOwnerScope::Project;
    std::function<void(EcsEntity, Components&..., SystemContext&)> run;
};

struct SystemSnapshot {
    SystemId id;
    std::string name;
    SystemKind kind = SystemKind::Native;
    bool enabled = true;
    SystemPhase phase = SystemPhase::Update;
    i32 order = 0;
    f32 interval_seconds = 0.0f;
    i32 rate = 1;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<SystemRelationAccess> relations;
    std::vector<SystemId> before;
    std::vector<SystemId> after;
    SystemExecutionPolicy execution_policy = SystemExecutionPolicy::MainThreadOnly;
    SystemStructuralMutationPolicy structural_mutation_policy = SystemStructuralMutationPolicy::None;
    bool parallel_eligible = false;
    std::string mutation_notes;
    SystemRunStats stats;
    SystemCommandStats command_stats;
    std::string last_error;
    std::vector<std::string> schedule_diagnostics;
    std::vector<SystemParallelDiagnostic> parallel_diagnostics;
    i32 batch_index = -1;
    SystemExecutionMode last_execution_mode = SystemExecutionMode::SerialGraph;
    SystemExecutionMode last_batch_execution_mode = SystemExecutionMode::SerialGraph;
    SystemExecutionDecision last_execution_decision = SystemExecutionDecision::Serial;
    std::string execution_decision_reason;
    i32 worker_count = 1;
    f64 estimated_work_ms = 0.0;
    std::string source_file;
    SystemOwnerScope owner_scope = SystemOwnerScope::Project;
};

struct SystemDependencyEdge {
    SystemId before;
    SystemId after;
    std::string reason;
};

struct SystemBatch {
    SystemPhase phase = SystemPhase::Update;
    i32 index = 0;
    std::vector<SystemId> systems;
    bool parallel_eligible = false;
    SystemExecutionMode execution_mode = SystemExecutionMode::SerialGraph;
    SystemExecutionDecision execution_decision = SystemExecutionDecision::Serial;
    std::string execution_decision_reason;
    i32 worker_count = 1;
    f64 estimated_work_ms = 0.0;
    f64 last_duration_ms = 0.0;
    SystemCommandStats command_stats;
    std::vector<std::string> diagnostics;
    std::vector<SystemParallelDiagnostic> parallel_diagnostics;
};

struct SystemScheduleSnapshot {
    bool valid = true;
    SystemExecutionMode requested_execution_mode = SystemExecutionMode::SerialGraph;
    SystemExecutionMode effective_execution_mode = SystemExecutionMode::SerialGraph;
    std::vector<std::string> diagnostics;
    std::vector<SystemDependencyEdge> edges;
    std::vector<SystemBatch> batches;
};

std::string_view system_phase_name(SystemPhase phase);
std::string_view system_execution_mode_name(SystemExecutionMode mode);
std::string_view system_execution_decision_name(SystemExecutionDecision decision);
std::array<SystemPhase, system_phase_count> system_phases();

namespace detail {

struct SystemCallbackState {
    virtual ~SystemCallbackState() = default;
    virtual void set_context(SystemContext context) = 0;
    virtual void run_task() {}
    virtual void clear_error() {}
    virtual std::string take_error() { return {}; }
};

template <typename... Components>
struct NativeSystemCallbackState final : SystemCallbackState {
    std::function<void(EcsEntity, Components&..., SystemContext&)> run;
    SystemContext context;
    std::mutex error_mutex;
    std::string error;

    void set_context(SystemContext next) override {
        context = next;
    }

    void clear_error() override {
        std::lock_guard lock{error_mutex};
        error.clear();
    }

    std::string take_error() override {
        std::lock_guard lock{error_mutex};
        return error;
    }

    void capture_error(std::string message) {
        std::lock_guard lock{error_mutex};
        if (error.empty()) {
            error = std::move(message);
        }
    }
};

inline std::set<std::string> normalized_access(const std::vector<std::string>& values) {
    std::set<std::string> result;
    for (const std::string& value : values) {
        if (!value.empty()) {
            result.insert(value);
        }
    }
    return result;
}

inline bool normalize_access_list(std::vector<std::string>& values,
                                  std::string_view system_id,
                                  std::string_view field,
                                  std::string& error) {
    std::set<std::string> seen;
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    for (const std::string& value : values) {
        if (value.empty()) {
            error = "system '" + std::string{system_id} + "' has an empty " + std::string{field} + " access name";
            return false;
        }
        if (!seen.insert(value).second) {
            error = "system '" + std::string{system_id} + "' declares duplicate " + std::string{field} + " access '" + value + "'";
            return false;
        }
        normalized.push_back(value);
    }
    values = std::move(normalized);
    return true;
}

template <typename Descriptor>
bool normalize_descriptor_access(Descriptor& descriptor, std::string& error) {
    return normalize_access_list(descriptor.reads, descriptor.id, "read", error) &&
           normalize_access_list(descriptor.writes, descriptor.id, "write", error);
}

template <typename Component>
std::string native_component_name(flecs::world& world) {
    using ComponentType = std::remove_cv_t<std::remove_reference_t<Component>>;
    const flecs::string_view value = world.component<ComponentType>().name();
    return {value.c_str(), value.length()};
}

template <typename T>
void append_component_access(flecs::world& world, std::vector<std::string>& access) {
    const std::string name = native_component_name<T>(world);
    if (!name.empty()) {
        access.push_back(name);
    }
}

template <typename... Components>
std::vector<std::string> component_access_names(flecs::world& world) {
    std::vector<std::string> result;
    (append_component_access<Components>(world, result), ...);
    return result;
}

// Derives access metadata from the template signature: const components are
// reads, mutable components are writes. Missing declarations are appended
// instead of rejected — the template parameter list is the single source of
// truth, and the string lists only need entries for accesses the callback
// performs outside its parameters (e.g. tags added via entity.add<T>()).
// The one contradiction (const component explicitly declared as a write) is
// still an error.
template <typename Component>
bool reconcile_native_component_access(flecs::world& world,
                                       std::vector<std::string>& reads,
                                       std::vector<std::string>& writes,
                                       const std::set<std::string>& declared_reads,
                                       const std::set<std::string>& declared_writes,
                                       std::string_view system_id,
                                       std::string& error) {
    using BareComponent = std::remove_reference_t<Component>;
    const std::string name = native_component_name<Component>(world);
    if (name.empty()) {
        error = "native system '" + std::string{system_id} + "' has an unnamed component access";
        return false;
    }

    if constexpr (std::is_const_v<BareComponent>) {
        if (declared_writes.contains(name)) {
            error = "native system '" + std::string{system_id} + "' declares read-only component '" + name + "' as a write";
            return false;
        }
        if (!declared_reads.contains(name)) {
            reads.push_back(name);
        }
    } else {
        if (!declared_writes.contains(name)) {
            writes.push_back(name);
        }
    }
    return true;
}

template <typename... Components>
bool reconcile_native_access(flecs::world& world,
                             std::vector<std::string>& reads,
                             std::vector<std::string>& writes,
                             std::string_view system_id,
                             std::string& error) {
    const std::set<std::string> declared_reads = normalized_access(reads);
    const std::set<std::string> declared_writes = normalized_access(writes);
    return (reconcile_native_component_access<Components>(world, reads, writes, declared_reads, declared_writes, system_id, error) && ...);
}

} // namespace detail

class EcsSystemRegistry {
public:
    explicit EcsSystemRegistry(EcsWorld& world);
    ~EcsSystemRegistry();

    EcsSystemRegistry(const EcsSystemRegistry&) = delete;
    EcsSystemRegistry& operator=(const EcsSystemRegistry&) = delete;

    template <typename... Components>
    SystemId register_native(NativeSystemDescriptor<Components...> descriptor,
                             std::source_location location = std::source_location::current()) {
        if (descriptor.id.empty()) {
            return registration_failure("system id is required", location);
        }
        if (!descriptor.run) {
            return registration_failure("native system '" + descriptor.id + "' has no run callback", location);
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
        if (!detail::reconcile_native_access<Components...>(_world->raw(), descriptor.reads, descriptor.writes, descriptor.id, access_error)) {
            return registration_failure(std::move(access_error), location);
        }

        auto state = std::make_shared<detail::NativeSystemCallbackState<Components...>>();
        state->run = std::move(descriptor.run);
        detail::NativeSystemCallbackState<Components...>* state_ptr = state.get();

        const std::string flecs_name = descriptor.name.empty() ? descriptor.id : descriptor.name;
        // multi_threaded is required for ecs_run_worker to actually partition
        // entities across workers; without it every worker runs the FULL query
        // range concurrently (flecs_run_intern gates ecs_worker_iter on it).
        flecs::system system = _world->raw()
                                   .system<Components...>(flecs_name.c_str())
                                   .kind(phase_entity(descriptor.phase).id())
                                   .multi_threaded(descriptor.execution_policy == SystemExecutionPolicy::ParallelEligible)
                                   .each([state_ptr](flecs::entity entity, Components&... components) {
                                       SystemContext context = state_ptr->context;
                                       try {
                                           state_ptr->run(EcsEntity{entity}, components..., context);
                                       } catch (const std::exception& error) {
                                           state_ptr->capture_error(error.what());
                                       } catch (...) {
                                           state_ptr->capture_error("unknown native system callback error");
                                       }
                                   });

        SystemDescriptor base{
            .id = std::move(descriptor.id),
            .name = std::move(descriptor.name),
            .kind = SystemKind::Native,
            .phase = descriptor.phase,
            .order = descriptor.order,
            .interval_seconds = descriptor.interval_seconds,
            .rate = descriptor.rate,
            .reads = std::move(descriptor.reads),
            .writes = std::move(descriptor.writes),
            .relations = std::move(descriptor.relations),
            .before = std::move(descriptor.before),
            .after = std::move(descriptor.after),
            .enabled = descriptor.enabled,
            .execution_policy = descriptor.execution_policy,
            .structural_mutation_policy = descriptor.structural_mutation_policy,
            .mutation_notes = std::move(descriptor.mutation_notes),
            .source_file = std::move(descriptor.source_file),
            .owner_scope = descriptor.owner_scope,
        };
        if (base.name.empty()) {
            base.name = base.id;
        }
        if (base.source_file.empty()) {
            base.source_file = location.file_name();
        }
        return register_created_system(std::move(base), system, std::move(state), location);
    }

    bool contains(std::string_view id) const;
    SystemId register_task(SystemDescriptor descriptor,
                           std::function<void(SystemContext&)> run,
                           std::source_location location = std::source_location::current());
    bool enable(std::string_view id, bool enabled = true);
    bool disable(std::string_view id);
    bool remove(std::string_view id);

    bool rebuild_schedule();
    const SystemScheduleSnapshot& schedule_snapshot() const { return _last_schedule; }
    bool run_system(std::string_view id, f32 dt = 0.0f);
    bool run_phase(SystemPhase phase, f32 dt = 0.0f);
    bool run_phase(SystemPhase phase, f32 dt, SystemExecutionMode mode);
    bool run_frame(f32 dt = 0.0f);
    bool run_frame(f32 dt, SystemExecutionMode mode);

    std::vector<SystemSnapshot> snapshots() const;
    SystemSnapshot snapshot(std::string_view id) const;

    const std::string& last_error() const { return _last_error; }
    const SystemExecutionTuning& execution_tuning() const { return _execution_tuning; }
    void set_execution_tuning(SystemExecutionTuning tuning) { _execution_tuning = tuning; }

    // Runs job(worker_index) for worker_index in [0, worker_count) on the
    // registry's job system and blocks until all jobs complete. For game code
    // that partitions its own data (snapshot compute passes). The job must not
    // touch the registry.
    void parallel_for_workers(i32 worker_count, const std::function<void(i32)>& job) {
        _jobs->parallel_for(worker_count, job);
    }

    // The job system parallel systems run on: default_job_system() unless set.
    // Set it before running systems; it must outlive the registry.
    JobSystem& job_system() const { return *_jobs; }
    void set_job_system(JobSystem& jobs) { _jobs = &jobs; }
    // Emits one human-readable execution-decision row per parallel-eligible
    // system into the sink ("exec.<id>" -> "parallel x16" / "serial: <reason>"),
    // so silently-serialized parallelism is visible in the debug overlay. No-op
    // when the sink is empty.
    void publish_execution_status(const std::function<void(std::string_view, std::string)>& sink) const;
    flecs::entity phase_entity(SystemPhase phase) const;

    template <typename... Components>
    std::vector<std::string> access_names() {
        return detail::component_access_names<Components...>(_world->raw());
    }

private:
    struct SystemRecord {
        SystemDescriptor descriptor;
        flecs::entity_t flecs_id = 0;
        std::shared_ptr<detail::SystemCallbackState> callback_state;
        std::source_location source_location;
        u64 registration_sequence = 0;
        bool enabled = true;
        u64 scheduled_ticks = 0;
        f32 interval_accumulator = 0.0f;
        SystemRunStats stats;
        f64 total_duration_ms = 0.0;
        SystemCommandStats command_stats;
        std::string last_error;
        std::vector<std::string> schedule_diagnostics;
        std::vector<SystemParallelDiagnostic> parallel_diagnostics;
        i32 batch_index = -1;
        bool parallel_eligible = false;
        SystemExecutionMode last_execution_mode = SystemExecutionMode::SerialGraph;
        SystemExecutionMode last_batch_execution_mode = SystemExecutionMode::SerialGraph;
        SystemExecutionDecision last_execution_decision = SystemExecutionDecision::Serial;
        std::string execution_decision_reason;
        i32 worker_count = 1;
        f64 estimated_work_ms = 0.0;
    };

    struct SystemRunResult {
        SystemRecord* record = nullptr;
        bool ok = true;
        i64 elapsed_ns = 0;
        std::string error;
        EcsCommandBuffer* commands = nullptr;
        SystemExecutionMode execution_mode = SystemExecutionMode::SerialGraph;
        SystemExecutionDecision decision = SystemExecutionDecision::Serial;
        std::string decision_reason;
        i32 worker_count = 1;
        f64 estimated_work_ms = 0.0;
    };

    SystemId register_created_system(SystemDescriptor descriptor,
                                     flecs::system system,
                                     std::shared_ptr<detail::SystemCallbackState> callback_state,
                                     std::source_location location);
    SystemRecord* find_record(std::string_view id);
    const SystemRecord* find_record(std::string_view id) const;
    SystemRunResult execute_record(SystemRecord& record,
                                   f32 dt,
                                   bool respect_schedule,
                                   EcsCommandBuffer* commands = nullptr,
                                   bool running_parallel = false,
                                   flecs::world* stage = nullptr);
    // Sets _last_error, logs the failure (registration errors were previously
    // silent and surfaced only as confusing schedule diagnostics), returns {}.
    SystemId registration_failure(std::string error, const std::source_location& location);
    SystemRunResult execute_native_parallel_record(SystemRecord& record,
                                                   f32 dt,
                                                   EcsCommandBuffer* commands);
    bool run_record(SystemRecord& record, f32 dt, bool respect_schedule);
    bool flush_commands_for_result(SystemScheduleSnapshot* schedule, SystemBatch* batch, const SystemRunResult& result);
    bool flush_command_results(SystemScheduleSnapshot& schedule, SystemBatch& batch, const std::vector<SystemRunResult>& results);
    bool scheduled_run_due(SystemRecord& record, f32 dt);
    void update_matched_count(SystemRecord& record) const;
    std::vector<SystemParallelDiagnostic> parallel_eligibility_reasons(const SystemRecord& record) const;
    SystemScheduleSnapshot build_schedule(const std::vector<SystemRecord*>& records,
                                          SystemExecutionMode requested_mode = SystemExecutionMode::SerialGraph) const;
    bool run_schedule(SystemScheduleSnapshot& schedule, f32 dt);
    bool run_parallel_batch(SystemScheduleSnapshot& schedule, SystemBatch& batch, f32 dt);
    bool collect_batch_records(const SystemBatch& batch, std::vector<SystemRecord*>& records, bool& has_native_system);
    void publish_parallel_results(SystemScheduleSnapshot& schedule,
                                  SystemBatch& batch,
                                  const std::vector<SystemRunResult>& results,
                                  bool& ok,
                                  bool& any_parallel);
    std::vector<SystemRecord*> eligible_records(SystemPhase* phase, f32 dt);
    std::vector<SystemRecord*> enabled_records(SystemPhase* phase = nullptr);
    void apply_schedule_diagnostics(const SystemScheduleSnapshot& schedule);
    std::vector<SystemRecord*> sorted_records(SystemPhase* phase = nullptr);
    std::vector<const SystemRecord*> sorted_records(SystemPhase* phase = nullptr) const;

    EcsWorld* _world = nullptr;
    std::vector<SystemRecord> _records;
    std::array<flecs::entity, system_phase_count> _phase_entities;
    u64 _next_registration_sequence = 0;
    std::string _last_error;
    SystemScheduleSnapshot _last_schedule;
    SystemExecutionTuning _execution_tuning;
    JobSystem* _jobs = nullptr;
};

} // namespace kin
