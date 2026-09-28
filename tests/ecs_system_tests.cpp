#include <kin/ecs/system.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Position {
    kin::f32 x = 0.0f;
};

struct Velocity {
    kin::f32 x = 0.0f;
};

struct Marker {
    kin::i32 value = 0;
};

struct EmptyTag {
};

struct Health {
    kin::i32 value = 0;
};

void register_components(kin::EcsWorld& world) {
    world.component<Position>("Position");
    world.component<Velocity>("Velocity");
    world.component<Marker>("Marker");
    world.component<EmptyTag>("EmptyTag");
    world.component<Health>("Health");
}

void register_component_metadata(kin::EcsWorld& world) {
    world.components().native<Position>("Position").field("x", &Position::x);
    world.components().native<Velocity>("Velocity").field("x", &Velocity::x);
    world.components().native<Marker>("Marker").field("value", &Marker::value);
    world.components().native<Health>("Health").field("value", &Health::value);
}

void register_access_metadata(kin::EcsWorld& world, std::initializer_list<std::string_view> names) {
    for (std::string_view name : names) {
        if (!world.components().find(name)) {
            world.components().data(std::string{name});
        }
    }
}

void test_native_system_runs_through_frame() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("moving").set(Position{1.0f}).set(Velocity{2.0f});

    const kin::SystemId id = world.systems().register_native<Position, const Velocity>({
        .id = "kin.test.move",
        .name = "Move",
        .phase = kin::SystemPhase::Update,
        .reads = {"Velocity"},
        .writes = {"Position"},
        .run = [](kin::EcsEntity, Position& position, const Velocity& velocity, kin::SystemContext& ctx) {
            position.x += velocity.x * ctx.dt;
        },
    });

    assert(id == "kin.test.move");
    assert(world.run_frame(0.5f));
    assert(entity.get<Position>()->x == 2.0f);

    const kin::SystemSnapshot snapshot = world.systems().snapshot(id);
    assert(snapshot.id == id);
    assert(snapshot.enabled);
    assert(snapshot.phase == kin::SystemPhase::Update);
    assert(snapshot.reads.size() == 1 && snapshot.reads[0] == "Velocity");
    assert(snapshot.writes.size() == 1 && snapshot.writes[0] == "Position");
    assert(snapshot.stats.runs == 1);
    assert(snapshot.stats.matched_entities == 1);
}

void test_phase_and_order_are_deterministic() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("marker").add<Marker>();

    std::vector<std::string> calls;
    const auto append = [&](std::string value) {
        return [&calls, value = std::move(value)](kin::EcsEntity, Marker&, kin::SystemContext&) {
            calls.push_back(value);
        };
    };

    world.systems().register_native<Marker>({
        .id = "update.second",
        .phase = kin::SystemPhase::Update,
        .order = 20,
        .writes = {"Marker"},
        .run = append("update.second"),
    });
    world.systems().register_native<Marker>({
        .id = "input.first",
        .phase = kin::SystemPhase::Input,
        .order = 100,
        .writes = {"Marker"},
        .run = append("input.first"),
    });
    world.systems().register_native<Marker>({
        .id = "update.first",
        .phase = kin::SystemPhase::Update,
        .order = 10,
        .writes = {"Marker"},
        .run = append("update.first"),
    });

    assert(world.run_frame(0.0f));
    assert((calls == std::vector<std::string>{"input.first", "update.first", "update.second"}));

    const std::vector<kin::SystemSnapshot> snapshots = world.systems().snapshots();
    assert(snapshots.size() == 3);
    assert(snapshots[0].id == "input.first");
    assert(snapshots[1].id == "update.first");
    assert(snapshots[2].id == "update.second");
}

void test_enable_disable_and_manual_run() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("moving").set(Position{0.0f}).set(Velocity{3.0f});

    world.systems().register_native<Position, const Velocity>({
        .id = "move.manual",
        .phase = kin::SystemPhase::Update,
        .reads = {"Velocity"},
        .writes = {"Position"},
        .run = [](kin::EcsEntity, Position& position, const Velocity& velocity, kin::SystemContext& ctx) {
            position.x += velocity.x * ctx.dt;
        },
    });

    assert(world.systems().disable("move.manual"));
    assert(!world.systems().snapshot("move.manual").enabled);
    assert(world.run_frame(1.0f));
    assert(entity.get<Position>()->x == 0.0f);

    assert(world.systems().enable("move.manual"));
    assert(world.run_system("move.manual", 2.0f));
    assert(entity.get<Position>()->x == 6.0f);
    assert(world.systems().snapshot("move.manual").stats.runs == 1);
}

void test_duplicate_ids_are_rejected() {
    kin::EcsWorld world;
    register_components(world);

    const kin::SystemId first = world.systems().register_native<Marker>({
        .id = "duplicate",
        .writes = {"Marker"},
        .run = [](kin::EcsEntity, Marker&, kin::SystemContext&) {},
    });
    const kin::SystemId second = world.systems().register_native<Marker>({
        .id = "duplicate",
        .writes = {"Marker"},
        .run = [](kin::EcsEntity, Marker&, kin::SystemContext&) {},
    });

    assert(first == "duplicate");
    assert(second.empty());
    assert(world.systems().last_error().find("duplicate") != std::string::npos);
    assert(world.systems().snapshots().size() == 1);
}

void test_remove_stops_future_execution() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("marker").add<Marker>();
    kin::i32 calls = 0;

    world.systems().register_native<Marker>({
        .id = "remove.me",
        .writes = {"Marker"},
        .run = [&calls](kin::EcsEntity, Marker&, kin::SystemContext&) {
            ++calls;
        },
    });

    assert(world.run_frame(0.0f));
    assert(calls == 1);
    assert(world.systems().remove("remove.me"));
    assert(!world.systems().contains("remove.me"));
    assert(world.run_frame(0.0f));
    assert(calls == 1);
    assert(world.systems().snapshot("remove.me").id.empty());
}

void test_empty_tags_are_supported_as_read_only_terms() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("tagged").add<EmptyTag>();
    kin::i32 calls = 0;

    world.systems().register_native<const EmptyTag>({
        .id = "tag.readonly",
        .reads = {"EmptyTag"},
        .run = [&calls](kin::EcsEntity, const EmptyTag&, kin::SystemContext&) {
            ++calls;
        },
    });

    assert(world.run_frame(0.0f));
    assert(calls == 1);
    assert(world.systems().snapshot("tag.readonly").stats.matched_entities == 1);
}

void test_rate_runs_every_nth_scheduled_tick_starting_immediately() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("marker").add<Marker>();
    kin::i32 calls = 0;

    world.systems().register_native<Marker>({
        .id = "rate.three",
        .rate = 3,
        .writes = {"Marker"},
        .run = [&calls](kin::EcsEntity, Marker&, kin::SystemContext&) {
            ++calls;
        },
    });

    assert(world.run_frame(0.0f));
    assert(world.run_frame(0.0f));
    assert(world.run_frame(0.0f));
    assert(world.run_frame(0.0f));

    assert(calls == 2);
    const kin::SystemSnapshot snapshot = world.systems().snapshot("rate.three");
    assert(snapshot.rate == 3);
    assert(snapshot.stats.runs == 2);
}

void test_interval_waits_for_accumulated_scheduled_dt() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("marker").add<Marker>();
    kin::i32 calls = 0;

    world.systems().register_native<Marker>({
        .id = "interval.half",
        .interval_seconds = 0.5f,
        .writes = {"Marker"},
        .run = [&calls](kin::EcsEntity, Marker&, kin::SystemContext&) {
            ++calls;
        },
    });

    assert(world.run_frame(0.2f));
    assert(calls == 0);
    assert(world.run_frame(0.2f));
    assert(calls == 0);
    assert(world.run_frame(0.1f));
    assert(calls == 1);
    assert(world.run_frame(0.49f));
    assert(calls == 1);
    assert(world.run_frame(0.01f));
    assert(calls == 2);

    const kin::SystemSnapshot snapshot = world.systems().snapshot("interval.half");
    assert(snapshot.interval_seconds == 0.5f);
    assert(snapshot.stats.runs == 2);
}

void test_manual_run_bypasses_rate_and_interval() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("marker").add<Marker>();
    kin::i32 calls = 0;

    world.systems().register_native<Marker>({
        .id = "manual.schedule",
        .interval_seconds = 10.0f,
        .rate = 10,
        .writes = {"Marker"},
        .run = [&calls](kin::EcsEntity, Marker&, kin::SystemContext&) {
            ++calls;
        },
    });

    assert(world.run_system("manual.schedule", 0.0f));
    assert(calls == 1);
    assert(world.systems().snapshot("manual.schedule").stats.runs == 1);
}

void test_native_access_validation_rejects_mismatches() {
    kin::EcsWorld world;
    register_components(world);

    const std::vector<std::string> typed_names = world.systems().access_names<Position, const Velocity>();
    assert((typed_names == std::vector<std::string>{"Position", "Velocity"}));

    // Access metadata is derived from the template signature: undeclared
    // mutable components become writes and undeclared const components become
    // reads, so a forgotten declaration can no longer silently reject a system.
    const kin::SystemId derived_write = world.systems().register_native<Position>({
        .id = "derived.write",
        .run = [](kin::EcsEntity, Position&, kin::SystemContext&) {},
    });
    assert(derived_write == "derived.write");
    const std::vector<std::string> derived_writes = world.systems().snapshot("derived.write").writes;
    assert(std::ranges::find(derived_writes, "Position") != derived_writes.end());

    const kin::SystemId derived_read = world.systems().register_native<const Velocity>({
        .id = "derived.read",
        .run = [](kin::EcsEntity, const Velocity&, kin::SystemContext&) {},
    });
    assert(derived_read == "derived.read");
    const std::vector<std::string> derived_reads = world.systems().snapshot("derived.read").reads;
    assert(std::ranges::find(derived_reads, "Velocity") != derived_reads.end());

    const kin::SystemId const_write = world.systems().register_native<const EmptyTag>({
        .id = "const.write",
        .writes = {"EmptyTag"},
        .run = [](kin::EcsEntity, const EmptyTag&, kin::SystemContext&) {},
    });
    assert(const_write.empty());
    assert(world.systems().last_error().find("read-only component 'EmptyTag'") != std::string::npos);

    const kin::SystemId extra_access = world.systems().register_native<Marker>({
        .id = "extra.access",
        .reads = {"ExternalClock"},
        .writes = {"Marker"},
        .run = [](kin::EcsEntity, Marker&, kin::SystemContext&) {},
    });
    assert(extra_access == "extra.access");

    const kin::SystemId empty_access = world.systems().register_native<Marker>({
        .id = "empty.access",
        .writes = {""},
        .run = [](kin::EcsEntity, Marker&, kin::SystemContext&) {},
    });
    assert(empty_access.empty());
    assert(world.systems().last_error().find("empty write access name") != std::string::npos);

    const kin::SystemId duplicate_access = world.systems().register_task({
        .id = "duplicate.access",
        .reads = {"Position", "Position"},
    }, [](kin::SystemContext&) {});
    assert(duplicate_access.empty());
    assert(world.systems().last_error().find("duplicate read access 'Position'") != std::string::npos);
}

const kin::SystemBatch* find_batch(const kin::SystemScheduleSnapshot& schedule, kin::SystemPhase phase, kin::i32 index) {
    const auto found = std::ranges::find_if(schedule.batches, [&](const kin::SystemBatch& batch) {
        return batch.phase == phase && batch.index == index;
    });
    return found == schedule.batches.end() ? nullptr : &*found;
}

bool has_edge(const kin::SystemScheduleSnapshot& schedule, std::string_view before, std::string_view after) {
    return std::ranges::any_of(schedule.edges, [&](const kin::SystemDependencyEdge& edge) {
        return edge.before == before && edge.after == after;
    });
}

void test_read_only_systems_share_a_batch() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "read.a",
        .reads = {"Position"},
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "read.b",
        .reads = {"Position"},
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    const kin::SystemBatch* batch = find_batch(schedule, kin::SystemPhase::Update, 0);
    assert(batch != nullptr);
    assert(batch->systems.size() == 2);
    assert(world.systems().snapshot("read.a").batch_index == 0);
    assert(world.systems().snapshot("read.b").batch_index == 0);
}

void test_access_conflicts_create_edges_and_batches() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "write.transform",
        .writes = {"Transform"},
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "read.transform",
        .reads = {"Transform"},
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(has_edge(schedule, "write.transform", "read.transform"));
    assert(find_batch(schedule, kin::SystemPhase::Update, 0)->systems.size() == 1);
    assert(find_batch(schedule, kin::SystemPhase::Update, 1)->systems.size() == 1);
}

void test_explicit_dependencies_and_cycles_are_reported() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "first",
        .reads = {"A"},
        .before = {"last"},
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "middle",
        .reads = {"B"},
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "last",
        .reads = {"C"},
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(has_edge(schedule, "first", "last"));
    assert(find_batch(schedule, kin::SystemPhase::Update, 0)->systems.size() == 2);
    assert(find_batch(schedule, kin::SystemPhase::Update, 1)->systems.size() == 1);

    kin::EcsWorld cycle_world;
    cycle_world.systems().register_task({
        .id = "a",
        .reads = {"A"},
        .before = {"b"},
    }, [](kin::SystemContext&) {});
    cycle_world.systems().register_task({
        .id = "b",
        .reads = {"B"},
        .before = {"a"},
    }, [](kin::SystemContext&) {});

    assert(!cycle_world.systems().rebuild_schedule());
    assert(!cycle_world.systems().schedule_snapshot().valid);
    assert(!cycle_world.systems().schedule_snapshot().diagnostics.empty());
}

void test_order_does_not_block_independent_batching() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "low",
        .order = 0,
        .writes = {"A"},
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "high",
        .order = 100,
        .writes = {"B"},
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemBatch* batch = find_batch(world.systems().schedule_snapshot(), kin::SystemPhase::Update, 0);
    assert(batch != nullptr);
    assert(batch->systems.size() == 2);
}

void test_rate_interval_and_disabled_filter_before_batching() {
    kin::EcsWorld world;
    kin::i32 fast = 0;
    kin::i32 slow = 0;
    kin::i32 disabled = 0;

    world.systems().register_task({
        .id = "fast",
        .reads = {"A"},
    }, [&fast](kin::SystemContext&) { ++fast; });
    world.systems().register_task({
        .id = "slow",
        .rate = 2,
        .reads = {"B"},
    }, [&slow](kin::SystemContext&) { ++slow; });
    world.systems().register_task({
        .id = "disabled",
        .reads = {"C"},
    }, [&disabled](kin::SystemContext&) { ++disabled; });
    assert(world.systems().disable("disabled"));

    assert(world.run_frame(0.0f));
    assert(world.run_frame(0.0f));
    assert(fast == 2);
    assert(slow == 1);
    assert(disabled == 0);

    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(std::ranges::any_of(schedule.batches, [](const kin::SystemBatch& batch) {
        return std::ranges::find(batch.systems, "fast") != batch.systems.end();
    }));
    assert(std::ranges::none_of(schedule.batches, [](const kin::SystemBatch& batch) {
        return std::ranges::find(batch.systems, "slow") != batch.systems.end() ||
               std::ranges::find(batch.systems, "disabled") != batch.systems.end();
    }));
}

void test_missing_access_metadata_isolates_systems_and_unknown_dependencies_fail() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "unsafe.a",
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "unsafe.b",
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(find_batch(schedule, kin::SystemPhase::Update, 0)->systems.size() == 1);
    assert(find_batch(schedule, kin::SystemPhase::Update, 1)->systems.size() == 1);
    assert(!world.systems().snapshot("unsafe.a").schedule_diagnostics.empty());

    kin::EcsWorld unknown_world;
    unknown_world.systems().register_task({
        .id = "depends",
        .reads = {"A"},
        .before = {"missing"},
    }, [](kin::SystemContext&) {});
    assert(!unknown_world.systems().rebuild_schedule());
    assert(!unknown_world.systems().schedule_snapshot().valid);
}

void test_execution_policy_defaults_to_main_thread_only() {
    kin::EcsWorld world;
    world.systems().register_task({
        .id = "default.threading",
        .reads = {"A"},
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemSnapshot snapshot = world.systems().snapshot("default.threading");
    assert(snapshot.execution_policy == kin::SystemExecutionPolicy::MainThreadOnly);
    assert(!snapshot.parallel_eligible);
    assert(!snapshot.schedule_diagnostics.empty());
    assert(snapshot.schedule_diagnostics[0].find("main-thread-only") != std::string::npos);

    const kin::SystemBatch* batch = find_batch(world.systems().schedule_snapshot(), kin::SystemPhase::Update, 0);
    assert(batch != nullptr);
    assert(!batch->parallel_eligible);
    assert(!batch->parallel_diagnostics.empty());
    assert(batch->parallel_diagnostics[0].code == "main_thread_only");
    assert(snapshot.parallel_diagnostics[0].code == "main_thread_only");
}

void test_parallel_eligible_systems_and_batches_are_reported() {
    kin::EcsWorld world;
    register_component_metadata(world);
    world.systems().register_task({
        .id = "parallel.read.a",
        .reads = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "parallel.read.b",
        .reads = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemBatch* batch = find_batch(world.systems().schedule_snapshot(), kin::SystemPhase::Update, 0);
    assert(batch != nullptr);
    assert(batch->systems.size() == 2);
    assert(batch->parallel_eligible);
    assert(world.systems().snapshot("parallel.read.a").parallel_eligible);
    assert(world.systems().snapshot("parallel.read.b").parallel_eligible);
}

void test_thread_safety_contract_validates_parallel_metadata() {
    kin::EcsWorld world;
    register_component_metadata(world);
    world.relations().relation("targets");

    const kin::SystemId unknown = world.systems().register_task({
        .id = "parallel.unknown",
        .reads = {"Missing"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    assert(unknown.empty());
    assert(world.systems().last_error().find("unknown component") != std::string::npos);

    const kin::SystemId raw = world.systems().register_task({
        .id = "parallel.raw",
        .reads = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .structural_mutation_policy = kin::SystemStructuralMutationPolicy::RawWorldAllowed,
    }, [](kin::SystemContext&) {});
    assert(raw.empty());
    assert(world.systems().last_error().find("raw world mutation") != std::string::npos);

    const kin::SystemId command = world.systems().register_task({
        .id = "parallel.commands",
        .reads = {"Position"},
        .writes = {"Marker"},
        .relations = {kin::read_relation("targets")},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .structural_mutation_policy = kin::SystemStructuralMutationPolicy::CommandBufferOnly,
    }, [](kin::SystemContext&) {});
    assert(command == "parallel.commands");
    const kin::SystemSnapshot snapshot = world.systems().snapshot(command);
    assert(snapshot.structural_mutation_policy == kin::SystemStructuralMutationPolicy::CommandBufferOnly);
    assert(snapshot.relations.size() == 1);
}

void test_relation_access_conflicts_create_schedule_edges() {
    kin::EcsWorld world;
    register_component_metadata(world);
    world.relations().relation("targets");

    world.systems().register_task({
        .id = "relation.write",
        .reads = {"Position"},
        .relations = {kin::write_relation("targets")},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "relation.read",
        .reads = {"Velocity"},
        .relations = {kin::read_relation("targets")},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(std::ranges::any_of(schedule.edges, [](const kin::SystemDependencyEdge& edge) {
        return edge.reason.find("relation access conflict: targets") != std::string::npos;
    }));
    assert(world.systems().snapshot("relation.write").batch_index != world.systems().snapshot("relation.read").batch_index);
}

void test_unsafe_parallel_eligible_system_is_isolated_and_not_eligible() {
    kin::EcsWorld world;
    register_access_metadata(world, {"A"});
    world.systems().register_task({
        .id = "unsafe.parallel",
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "safe.parallel",
        .reads = {"A"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});

    assert(world.systems().rebuild_schedule());
    const kin::SystemSnapshot unsafe = world.systems().snapshot("unsafe.parallel");
    const kin::SystemSnapshot safe = world.systems().snapshot("safe.parallel");
    assert(!unsafe.parallel_eligible);
    assert(safe.parallel_eligible);
    assert(unsafe.batch_index != safe.batch_index);
    assert(std::ranges::any_of(unsafe.schedule_diagnostics, [](const std::string& diagnostic) {
        return diagnostic.find("missing access metadata") != std::string::npos;
    }));
}

void test_parallel_batches_runs_eligible_task_systems_concurrently() {
    kin::EcsWorld world;
    register_access_metadata(world, {"A", "B"});
    std::atomic<kin::i32> active = 0;
    std::atomic<kin::i32> max_active = 0;
    std::atomic<kin::i32> calls = 0;

    const auto run = [&](kin::SystemContext&) {
        const kin::i32 now = active.fetch_add(1) + 1;
        kin::i32 observed = max_active.load();
        while (now > observed && !max_active.compare_exchange_weak(observed, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        active.fetch_sub(1);
        calls.fetch_add(1);
    };

    world.systems().register_task({
        .id = "parallel.task.a",
        .reads = {"A"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, run);
    world.systems().register_task({
        .id = "parallel.task.b",
        .reads = {"B"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, run);

    assert(world.systems().run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(calls == 2);
    assert(max_active.load() > 1);
    assert(schedule.requested_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(schedule.effective_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(schedule.batches[0].execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(schedule.batches[0].execution_decision == kin::SystemExecutionDecision::ParallelAcrossSystems);
    assert(schedule.batches[0].worker_count == 2);
    assert(schedule.batches[0].last_duration_ms >= 0.0);
    assert(world.systems().snapshot("parallel.task.a").last_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().snapshot("parallel.task.a").last_execution_decision == kin::SystemExecutionDecision::ParallelAcrossSystems);
    assert(world.systems().snapshot("parallel.task.a").last_batch_execution_mode == kin::SystemExecutionMode::ParallelBatches);
}

void test_parallel_batches_keep_ineligible_batches_serial() {
    kin::EcsWorld world;
    std::vector<std::string> calls;
    world.systems().register_task({
        .id = "serial.default.a",
        .reads = {"A"},
    }, [&calls](kin::SystemContext&) {
        calls.push_back("a");
    });
    world.systems().register_task({
        .id = "serial.default.b",
        .reads = {"B"},
    }, [&calls](kin::SystemContext&) {
        calls.push_back("b");
    });

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert((calls == std::vector<std::string>{"a", "b"}));
    assert(world.systems().schedule_snapshot().effective_execution_mode == kin::SystemExecutionMode::SerialGraph);
}

void test_parallel_worker_errors_fail_frame_and_update_snapshot() {
    kin::EcsWorld world;
    register_access_metadata(world, {"A", "B"});
    world.systems().register_task({
        .id = "parallel.ok",
        .reads = {"A"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "parallel.fail",
        .reads = {"B"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {
        throw std::runtime_error{"parallel failure"};
    });

    assert(!world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(world.systems().schedule_snapshot().effective_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().schedule_snapshot().batches[0].execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().last_error().find("parallel failure") != std::string::npos);
    assert(world.systems().snapshot("parallel.fail").last_error.find("parallel failure") != std::string::npos);
    assert(std::ranges::any_of(world.systems().schedule_snapshot().diagnostics, [](const std::string& diagnostic) {
        return diagnostic.find("parallel system 'parallel.fail' failed") != std::string::npos;
    }));
}

void test_parallel_batches_run_eligible_native_systems_concurrently() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.systems().set_execution_tuning({
        .min_parallel_systems = 2,
        .min_native_entities = 1,
        .max_workers = 4,
    });
    std::vector<kin::EcsEntity> position_entities;
    std::vector<kin::EcsEntity> health_entities;
    for (kin::i32 i = 0; i < 8; ++i) {
        position_entities.push_back(world.entity("native.position." + std::to_string(i)).set(Position{}));
        health_entities.push_back(world.entity("native.health." + std::to_string(i)).set(Health{}));
    }
    std::atomic<kin::i32> active = 0;
    std::atomic<kin::i32> max_active = 0;

    const auto enter_parallel_work = [&] {
        const kin::i32 now = active.fetch_add(1) + 1;
        kin::i32 observed = max_active.load();
        while (now > observed && !max_active.compare_exchange_weak(observed, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        active.fetch_sub(1);
    };

    world.systems().register_native<Position>({
        .id = "native.position",
        .writes = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [&](kin::EcsEntity, Position& position, kin::SystemContext& ctx) {
            assert(ctx.running_parallel);
            enter_parallel_work();
            position.x = 3.0f;
        },
    });
    world.systems().register_native<Health>({
        .id = "native.health",
        .writes = {"Health"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [&](kin::EcsEntity, Health& health, kin::SystemContext& ctx) {
            assert(ctx.running_parallel);
            enter_parallel_work();
            health.value = 7;
        },
    });

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(max_active.load() > 1);
    for (kin::EcsEntity entity : position_entities) {
        assert(entity.get<Position>()->x == 3.0f);
    }
    for (kin::EcsEntity entity : health_entities) {
        assert(entity.get<Health>()->value == 7);
    }
    assert(world.systems().schedule_snapshot().effective_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().schedule_snapshot().batches[0].execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().schedule_snapshot().batches[0].execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
    assert(world.systems().snapshot("native.position").parallel_eligible);
    assert(world.systems().snapshot("native.health").parallel_eligible);
    assert(world.systems().snapshot("native.position").last_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.systems().snapshot("native.position").last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
    assert(world.systems().snapshot("native.position").worker_count > 1);
    assert(world.systems().snapshot("native.health").last_execution_mode == kin::SystemExecutionMode::ParallelBatches);
}

void test_small_native_workload_chooses_serial_by_default() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.entity("native.small").set(Position{});

    world.systems().register_native<Position>({
        .id = "native.small",
        .writes = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [](kin::EcsEntity, Position& position, kin::SystemContext& ctx) {
            assert(!ctx.running_parallel);
            position.x = 9.0f;
        },
    });

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    const kin::SystemSnapshot snapshot = world.systems().snapshot("native.small");
    assert(snapshot.parallel_eligible);
    assert(snapshot.last_execution_mode == kin::SystemExecutionMode::SerialGraph);
    assert(snapshot.last_execution_decision == kin::SystemExecutionDecision::Serial);
    assert(snapshot.execution_decision_reason.find("entity count below threshold") != std::string::npos);
    assert(world.systems().schedule_snapshot().effective_execution_mode == kin::SystemExecutionMode::SerialGraph);
}

void test_mixed_native_task_batch_reports_actual_decisions() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.systems().set_execution_tuning({
        .min_parallel_systems = 2,
        .min_native_entities = 1,
        .max_workers = 4,
    });
    for (kin::i32 i = 0; i < 8; ++i) {
        world.entity("mixed.native." + std::to_string(i)).set(Position{});
    }
    kin::i32 task_calls = 0;

    world.systems().register_native<Position>({
        .id = "mixed.native",
        .writes = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [](kin::EcsEntity, Position& position, kin::SystemContext& ctx) {
            assert(ctx.running_parallel);
            position.x = 2.0f;
        },
    });
    world.systems().register_task({
        .id = "mixed.task",
        .writes = {"Marker"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [&task_calls](kin::SystemContext& ctx) {
        assert(!ctx.running_parallel);
        ++task_calls;
    });

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(task_calls == 1);
    const kin::SystemSnapshot native = world.systems().snapshot("mixed.native");
    const kin::SystemSnapshot task = world.systems().snapshot("mixed.task");
    assert(native.last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
    assert(native.last_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(task.last_execution_decision == kin::SystemExecutionDecision::Serial);
    assert(task.last_execution_mode == kin::SystemExecutionMode::SerialGraph);
    assert(task.last_batch_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(task.execution_decision_reason.find("mixed native/task") != std::string::npos);
}

void test_native_parallel_failure_restores_stage_state() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.systems().set_execution_tuning({
        .min_parallel_systems = 2,
        .min_native_entities = 1,
        .max_workers = 4,
    });
    for (kin::i32 i = 0; i < 8; ++i) {
        world.entity("native.fail." + std::to_string(i)).set(Position{});
    }
    const kin::i32 before_stages = world.raw().get_stage_count();

    world.systems().register_native<Position>({
        .id = "native.fail",
        .writes = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [](kin::EcsEntity, Position&, kin::SystemContext& ctx) {
            assert(ctx.running_parallel);
            throw std::runtime_error{"native worker failure"};
        },
    });

    assert(!world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(world.systems().schedule_snapshot().effective_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(world.raw().get_stage_count() == before_stages);
    assert(world.systems().last_error().find("native worker failure") != std::string::npos);
    assert(world.systems().snapshot("native.fail").last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
}

void test_native_parallel_callbacks_receive_isolated_context() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.systems().set_execution_tuning({
        .min_parallel_systems = 2,
        .min_native_entities = 1,
        .max_workers = 4,
    });
    for (kin::i32 i = 0; i < 16; ++i) {
        world.entity("native.context." + std::to_string(i)).set(Position{});
    }
    std::atomic<kin::i32> calls = 0;
    std::atomic<bool> saw_mutated_context = false;

    world.systems().register_native<Position>({
        .id = "native.context",
        .writes = {"Position"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [&calls, &saw_mutated_context](kin::EcsEntity, Position& position, kin::SystemContext& ctx) {
            assert(ctx.world != nullptr);
            assert(ctx.commands == nullptr);
            assert(ctx.running_parallel);
            if (ctx.system_id != "native.context") {
                saw_mutated_context.store(true);
            }
            ctx.system_id = "mutated.context";
            position.x = 4.0f;
            calls.fetch_add(1);
        },
    });

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(calls.load() > 1);
    assert(!saw_mutated_context.load());
    const kin::SystemSnapshot snapshot = world.systems().snapshot("native.context");
    assert(snapshot.last_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(snapshot.last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
    assert(snapshot.last_error.empty());
}

void test_disabled_and_not_due_systems_are_excluded_from_parallel_analysis() {
    kin::EcsWorld world;
    register_access_metadata(world, {"A", "B", "C"});
    world.systems().register_task({
        .id = "included.parallel",
        .reads = {"A"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "not.due.parallel",
        .rate = 2,
        .reads = {"B"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    world.systems().register_task({
        .id = "disabled.parallel",
        .reads = {"C"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext&) {});
    assert(world.systems().disable("disabled.parallel"));

    assert(world.run_frame(0.0f));
    assert(world.run_frame(0.0f));

    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(std::ranges::any_of(schedule.batches, [](const kin::SystemBatch& batch) {
        return std::ranges::find(batch.systems, "included.parallel") != batch.systems.end();
    }));
    assert(std::ranges::none_of(schedule.batches, [](const kin::SystemBatch& batch) {
        return std::ranges::find(batch.systems, "not.due.parallel") != batch.systems.end() ||
               std::ranges::find(batch.systems, "disabled.parallel") != batch.systems.end();
    }));
    assert(!world.systems().snapshot("not.due.parallel").parallel_eligible);
    assert(!world.systems().snapshot("disabled.parallel").parallel_eligible);
}

void test_serial_task_commands_create_deferred_entity_after_batch() {
    kin::EcsWorld world;
    register_components(world);

    world.systems().register_task({
        .id = "commands.create",
        .writes = {"Position"},
    }, [](kin::SystemContext& ctx) {
        assert(ctx.commands != nullptr);
        assert(!ctx.running_parallel);
        const kin::EcsDeferredEntity entity = ctx.commands->create_entity("command.created");
        ctx.commands->set<Position>(entity, Position{5.0f});
    });

    assert(world.run_frame(0.0f));
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(!schedule.batches.empty());
    assert(schedule.batches[0].execution_mode == kin::SystemExecutionMode::SerialGraph);
    assert(schedule.batches[0].command_stats.queued == 2);
    assert(schedule.batches[0].command_stats.flushed == 2);
    const kin::SystemSnapshot command_snapshot = world.systems().snapshot("commands.create");
    assert(command_snapshot.command_stats.queued == 2);
    assert(command_snapshot.command_stats.flushed == 2);
    assert(command_snapshot.last_execution_mode == kin::SystemExecutionMode::SerialGraph);
    assert(world.count<Position>() == 1);
    kin::f32 x = 0.0f;
    world.query<Position>().each_entity([&](kin::EcsEntity entity, Position& position) {
        assert(entity.name() == "command.created");
        x = position.x;
    });
    assert(x == 5.0f);
}

void test_parallel_task_commands_flush_in_system_order() {
    kin::EcsWorld world;
    register_components(world);
    register_access_metadata(world, {"A", "B", "SpawnA", "SpawnB"});
    std::atomic<kin::i32> active = 0;
    std::atomic<kin::i32> max_active = 0;

    const auto run = [&](kin::i32 value) {
        return [&, value](kin::SystemContext& ctx) {
            assert(ctx.commands != nullptr);
            assert(ctx.running_parallel);
            const kin::i32 now = active.fetch_add(1) + 1;
            kin::i32 observed = max_active.load();
            while (now > observed && !max_active.compare_exchange_weak(observed, now)) {
            }
            const kin::EcsDeferredEntity entity = ctx.commands->create_entity(value == 1 ? "command.a" : "command.b");
            ctx.commands->set<Marker>(entity, Marker{value});
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            active.fetch_sub(1);
        };
    };

    world.systems().register_task({
        .id = "commands.parallel.a",
        .reads = {"A"},
        .writes = {"SpawnA"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, run(1));
    world.systems().register_task({
        .id = "commands.parallel.b",
        .reads = {"B"},
        .writes = {"SpawnB"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, run(2));

    assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(max_active.load() > 1);
    const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
    assert(schedule.effective_execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(schedule.batches[0].execution_mode == kin::SystemExecutionMode::ParallelBatches);
    assert(schedule.batches[0].command_stats.queued == 4);
    assert(schedule.batches[0].command_stats.flushed == 4);
    assert(world.systems().snapshot("commands.parallel.a").command_stats.queued == 2);
    assert(world.systems().snapshot("commands.parallel.a").command_stats.flushed == 2);
    std::vector<std::pair<kin::EcsId, kin::i32>> values;
    world.query<Marker>().each_entity([&](kin::EcsEntity entity, Marker& marker) {
        values.push_back({entity.id(), marker.value});
    });
    std::ranges::sort(values);
    assert(values.size() == 2);
    assert(values[0].second == 1);
    assert(values[1].second == 2);
}

void test_command_buffer_entity_operations() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("command.target").set(Position{1.0f}).set(Velocity{2.0f});
    const kin::EcsId entity_id = entity.id();

    world.systems().register_task({
        .id = "commands.ops",
        .writes = {"Position", "Marker", "Velocity"},
    }, [entity_id](kin::SystemContext& ctx) {
        ctx.commands->set<Position>(entity_id, Position{8.0f});
        ctx.commands->add<Marker>(entity_id);
        ctx.commands->remove<Velocity>(entity_id);
        ctx.commands->modified<Position>(entity_id);
    });

    assert(world.run_frame(0.0f));
    assert(entity.get<Position>()->x == 8.0f);
    assert(entity.has<Marker>());
    assert(!entity.has<Velocity>());

    world.systems().register_task({
        .id = "commands.destroy",
        .writes = {"EntityLifetime"},
    }, [entity_id](kin::SystemContext& ctx) {
        ctx.commands->destroy(entity_id);
    });
    assert(world.run_system("commands.destroy", 0.0f));
    assert(!entity.alive());
}

// Non-idempotent partition coverage: every matched entity must be processed
// exactly once per frame. Idempotent writes (the older tests) cannot detect a
// worker that runs the full range or a partition that gets dropped — this one
// fails on either, in both directions.
void test_native_parallel_partitions_each_entity_exactly_once() {
    kin::EcsWorld world;
    register_components(world);
    register_component_metadata(world);
    world.systems().set_execution_tuning({
        .min_parallel_systems = 2,
        .min_native_entities = 64,
        .max_workers = 8,
    });
    constexpr kin::i32 entity_count = 2048;
    std::vector<kin::EcsEntity> marker_entities;
    marker_entities.reserve(entity_count);
    for (kin::i32 i = 0; i < entity_count; ++i) {
        kin::EcsEntity entity = world.entity("parallel.once." + std::to_string(i)).set(Marker{0});
        // Spread entities across several archetypes so the query spans
        // multiple tables and partitioning is exercised per table.
        if (i % 3 == 0) {
            entity.add<EmptyTag>();
        }
        if (i % 5 == 0) {
            entity.set(Health{i});
        }
        marker_entities.push_back(entity);
    }
    world.systems().register_native<Marker>({
        .id = "parallel.once.marker",
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [](kin::EcsEntity, Marker& marker, kin::SystemContext&) {
            marker.value += 1;
        },
    });
    world.systems().register_native<Position>({
        .id = "parallel.once.position",
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [](kin::EcsEntity, Position& position, kin::SystemContext&) {
            position.x += 1.0f;
        },
    });
    for (kin::i32 i = 0; i < 256; ++i) {
        world.entity("parallel.once.pos." + std::to_string(i)).set(Position{0.0f});
    }

    constexpr kin::i32 frames = 4;
    for (kin::i32 frame = 0; frame < frames; ++frame) {
        assert(world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    }
    // Guard against the test silently passing because a threshold serialized
    // the system: the parallel path itself must have engaged.
    if (world.systems().snapshot("parallel.once.marker").last_execution_decision != kin::SystemExecutionDecision::ParallelWithinNativeSystem) {
        std::fprintf(stderr, "DEBUG decision reason: %s\n", world.systems().snapshot("parallel.once.marker").execution_decision_reason.c_str());
    }
    assert(world.systems().snapshot("parallel.once.marker").last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem);
    assert(world.systems().snapshot("parallel.once.marker").worker_count > 1);
    for (kin::EcsEntity& entity : marker_entities) {
        const Marker* marker = entity.get<Marker>();
        assert(marker != nullptr);
        assert(marker->value == frames);
    }
}

// Determinism coverage: a parallel native that performs per-entity math plus
// deferred structural ops (tag add/remove) must yield bit-identical state
// run-to-run and serial-vs-parallel. Catches nondeterministic partitioning,
// stage-merge ordering changes, and races on deferred queues.
void test_native_parallel_is_deterministic() {
    const auto run_simulation = [](kin::SystemExecutionMode mode) {
        kin::EcsWorld world;
        register_components(world);
        register_component_metadata(world);
        world.systems().set_execution_tuning({
            .min_parallel_systems = 2,
            .min_native_entities = 64,
            .max_workers = 8,
        });
        constexpr kin::i32 entity_count = 1024;
        std::vector<kin::EcsEntity> entities;
        entities.reserve(entity_count);
        for (kin::i32 i = 0; i < entity_count; ++i) {
            entities.push_back(world.entity("parallel.det." + std::to_string(i)).set(Marker{i % 17}));
        }
        world.systems().register_native<Marker>({
            .id = "parallel.det.step",
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
            .run = [](kin::EcsEntity entity, Marker& marker, kin::SystemContext&) {
                marker.value = marker.value * 31 + 7;
                if (marker.value % 2 == 0) {
                    entity.add<EmptyTag>();
                } else {
                    entity.remove<EmptyTag>();
                }
            },
        });
        world.systems().register_native<Position>({
            .id = "parallel.det.companion",
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
            .run = [](kin::EcsEntity, Position& position, kin::SystemContext&) {
                position.x += 0.5f;
            },
        });
        for (kin::i32 i = 0; i < 128; ++i) {
            world.entity("parallel.det.pos." + std::to_string(i)).set(Position{static_cast<kin::f32>(i)});
        }
        for (kin::i32 frame = 0; frame < 6; ++frame) {
            assert(world.run_frame(0.0f, mode));
        }
        kin::u64 hash = 1469598103934665603ull;
        const auto mix = [&hash](kin::u64 value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Marker* marker = entities[i].get<Marker>();
            assert(marker != nullptr);
            mix(static_cast<kin::u64>(i));
            mix(static_cast<kin::u64>(static_cast<kin::u32>(marker->value)));
            mix(entities[i].has<EmptyTag>() ? 1u : 0u);
        }
        return hash;
    };

    const kin::u64 parallel_a = run_simulation(kin::SystemExecutionMode::ParallelBatches);
    const kin::u64 parallel_b = run_simulation(kin::SystemExecutionMode::ParallelBatches);
    const kin::u64 serial = run_simulation(kin::SystemExecutionMode::SerialGraph);
    assert(parallel_a == parallel_b);
    assert(parallel_a == serial);
}

// Pool stress: every index of every run must execute exactly once across
// repeated runs with varying counts (guards the worker pool's generation and
// work-stealing logic against stale-worker and lost-job bugs).
void test_parallel_for_workers_runs_each_index_exactly_once() {
    kin::EcsWorld world;
    for (const kin::i32 count : {1, 2, 7, 32, 96}) {
        for (kin::i32 repetition = 0; repetition < 25; ++repetition) {
            std::deque<std::atomic<kin::i32>> hits(static_cast<std::size_t>(count));
            world.systems().parallel_for_workers(count, [&](kin::i32 index) {
                hits[static_cast<std::size_t>(index)].fetch_add(1);
            });
            for (const std::atomic<kin::i32>& hit : hits) {
                assert(hit.load() == 1);
            }
        }
    }
}

void test_failed_parallel_worker_command_buffer_is_not_flushed() {
    kin::EcsWorld world;
    register_components(world);
    register_access_metadata(world, {"A", "B", "SpawnA", "SpawnB"});

    world.systems().register_task({
        .id = "commands.ok",
        .reads = {"A"},
        .writes = {"SpawnA"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext& ctx) {
        const kin::EcsDeferredEntity entity = ctx.commands->create_entity("command.ok");
        ctx.commands->set<Marker>(entity, Marker{10});
    });
    world.systems().register_task({
        .id = "commands.fail",
        .reads = {"B"},
        .writes = {"SpawnB"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [](kin::SystemContext& ctx) {
        const kin::EcsDeferredEntity entity = ctx.commands->create_entity("command.fail");
        ctx.commands->set<Marker>(entity, Marker{99});
        throw std::runtime_error{"command worker failure"};
    });

    assert(!world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches));
    assert(world.systems().snapshot("commands.ok").command_stats.flushed == 2);
    assert(world.systems().snapshot("commands.fail").command_stats.discarded == 2);
    assert(world.systems().schedule_snapshot().batches[0].command_stats.discarded == 2);
    assert(world.count<Marker>() == 1);
    kin::i32 value = 0;
    world.query<Marker>().each_entity([&](kin::EcsEntity entity, Marker& marker) {
        assert(entity.name() == "command.ok");
        value = marker.value;
    });
    assert(value == 10);
}

void test_run_system_provides_and_flushes_command_buffer() {
    kin::EcsWorld world;
    register_components(world);

    world.systems().register_task({
        .id = "commands.manual",
        .writes = {"Health"},
    }, [](kin::SystemContext& ctx) {
        assert(ctx.commands != nullptr);
        const kin::EcsDeferredEntity entity = ctx.commands->create_entity("command.manual");
        ctx.commands->set<Health>(entity, Health{42});
    });

    assert(world.run_system("commands.manual", 0.0f));
    assert(world.systems().snapshot("commands.manual").command_stats.queued == 2);
    assert(world.systems().snapshot("commands.manual").command_stats.flushed == 2);
    assert(world.count<Health>() == 1);
    world.query<Health>().each_entity([](kin::EcsEntity entity, Health& health) {
        assert(entity.name() == "command.manual");
        assert(health.value == 42);
    });
}

} // namespace

int main() {
    test_native_system_runs_through_frame();
    test_phase_and_order_are_deterministic();
    test_enable_disable_and_manual_run();
    test_duplicate_ids_are_rejected();
    test_remove_stops_future_execution();
    test_empty_tags_are_supported_as_read_only_terms();
    test_rate_runs_every_nth_scheduled_tick_starting_immediately();
    test_interval_waits_for_accumulated_scheduled_dt();
    test_manual_run_bypasses_rate_and_interval();
    test_native_access_validation_rejects_mismatches();
    test_read_only_systems_share_a_batch();
    test_access_conflicts_create_edges_and_batches();
    test_explicit_dependencies_and_cycles_are_reported();
    test_order_does_not_block_independent_batching();
    test_rate_interval_and_disabled_filter_before_batching();
    test_missing_access_metadata_isolates_systems_and_unknown_dependencies_fail();
    test_execution_policy_defaults_to_main_thread_only();
    test_parallel_eligible_systems_and_batches_are_reported();
    test_thread_safety_contract_validates_parallel_metadata();
    test_relation_access_conflicts_create_schedule_edges();
    test_unsafe_parallel_eligible_system_is_isolated_and_not_eligible();
    test_parallel_batches_runs_eligible_task_systems_concurrently();
    test_parallel_batches_keep_ineligible_batches_serial();
    test_parallel_worker_errors_fail_frame_and_update_snapshot();
    test_parallel_batches_run_eligible_native_systems_concurrently();
    test_native_parallel_partitions_each_entity_exactly_once();
    test_native_parallel_is_deterministic();
    test_parallel_for_workers_runs_each_index_exactly_once();
    test_small_native_workload_chooses_serial_by_default();
    test_mixed_native_task_batch_reports_actual_decisions();
    test_native_parallel_failure_restores_stage_state();
    test_native_parallel_callbacks_receive_isolated_context();
    test_disabled_and_not_due_systems_are_excluded_from_parallel_analysis();
    test_serial_task_commands_create_deferred_entity_after_batch();
    test_parallel_task_commands_flush_in_system_order();
    test_command_buffer_entity_operations();
    test_failed_parallel_worker_command_buffer_is_not_flushed();
    test_run_system_provides_and_flushes_command_buffer();
    return 0;
}
