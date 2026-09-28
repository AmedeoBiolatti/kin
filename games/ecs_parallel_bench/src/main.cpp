#include <kin/core/json.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/system.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct WorkA {
    kin::f32 value = 0.0f;
};

struct WorkB {
    kin::f32 value = 0.0f;
};

struct WorkC {
    kin::f32 value = 0.0f;
};

struct WorkD {
    kin::f32 value = 0.0f;
};

struct Options {
    kin::i32 frames = 6;
    kin::i32 warmup = 1;
    kin::i32 entities = 768;
    kin::i32 iterations = 1536;
    kin::f64 min_speedup = 0.0;
};

struct BenchResult {
    kin::f64 serial_ms = 0.0;
    kin::f64 parallel_ms = 0.0;
    kin::f64 speedup = 0.0;
    kin::f64 checksum = 0.0;
    kin::i32 parallel_batches = 0;
    kin::i32 native_parallel_systems = 0;
    kin::i32 serial_decisions = 0;
    kin::i32 parallel_across_systems_decisions = 0;
    kin::i32 parallel_within_native_system_decisions = 0;
    kin::i32 max_worker_count = 1;
};

struct Report {
    BenchResult task;
    BenchResult native;
};

struct TaskState {
    std::vector<kin::f32> a;
    std::vector<kin::f32> b;
    std::vector<kin::f32> c;
    std::vector<kin::f32> d;
    kin::i32 iterations = 1;
};

bool parse_int_arg(std::string_view arg, std::string_view name, kin::i32& value) {
    const std::string prefix = "--" + std::string{name} + "=";
    if (!arg.starts_with(prefix)) {
        return false;
    }
    value = std::max(0, std::atoi(std::string{arg.substr(prefix.size())}.c_str()));
    return true;
}

bool parse_double_arg(std::string_view arg, std::string_view name, kin::f64& value) {
    const std::string prefix = "--" + std::string{name} + "=";
    if (!arg.starts_with(prefix)) {
        return false;
    }
    value = std::max<kin::f64>(0.0, std::atof(std::string{arg.substr(prefix.size())}.c_str()));
    return true;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (parse_int_arg(arg, "frames", options.frames) ||
            parse_int_arg(arg, "warmup", options.warmup) ||
            parse_int_arg(arg, "entities", options.entities) ||
            parse_int_arg(arg, "iterations", options.iterations) ||
            parse_double_arg(arg, "min-speedup", options.min_speedup)) {
            continue;
        }
        if (arg == "--help") {
            std::cout << "ecs_parallel_bench [--frames=N] [--warmup=N] [--entities=N] [--iterations=N] [--min-speedup=N]\n";
            std::exit(0);
        }
    }
    options.frames = std::max(options.frames, 1);
    options.entities = std::max(options.entities, 1);
    options.iterations = std::max(options.iterations, 1);
    return options;
}

kin::f32 expensive_step(kin::f32 value, kin::f32 seed, kin::i32 iterations) {
    kin::f32 x = value + seed;
    for (kin::i32 i = 0; i < iterations; ++i) {
        const kin::f32 t = seed + static_cast<kin::f32>(i) * 0.00017f;
        x = std::sin(x * 0.013f + t) * 97.0f + std::cos(x * 0.021f - t) * 31.0f;
        x += static_cast<kin::f32>((i % 11) - 5) * 0.001f;
    }
    return x;
}

template <typename Component>
void register_work_system(kin::EcsWorld& world,
                          std::string id,
                          std::string component_name,
                          kin::f32 seed,
                          kin::i32 iterations) {
    world.systems().register_native<Component>({
        .id = std::move(id),
        .phase = kin::SystemPhase::Update,
        .reads = {},
        .writes = {component_name},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        .run = [seed, iterations](kin::EcsEntity, Component& work, kin::SystemContext& ctx) {
            const kin::f32 parallel_bias = ctx.running_parallel ? 0.00001f : 0.0f;
            work.value = expensive_step(work.value + parallel_bias, seed, iterations);
        },
    });
}

void register_components(kin::EcsWorld& world) {
    world.components().native<WorkA>("WorkA").field("value", &WorkA::value);
    world.components().native<WorkB>("WorkB").field("value", &WorkB::value);
    world.components().native<WorkC>("WorkC").field("value", &WorkC::value);
    world.components().native<WorkD>("WorkD").field("value", &WorkD::value);
}

void populate_world(kin::EcsWorld& world, const Options& options) {
    for (kin::i32 i = 0; i < options.entities; ++i) {
        const kin::f32 base = static_cast<kin::f32>(i) * 0.01f;
        world.entity("work.a." + std::to_string(i)).set(WorkA{base + 1.0f});
        world.entity("work.b." + std::to_string(i)).set(WorkB{base + 2.0f});
        world.entity("work.c." + std::to_string(i)).set(WorkC{base + 3.0f});
        world.entity("work.d." + std::to_string(i)).set(WorkD{base + 4.0f});
    }
}

kin::EcsWorld make_world(const Options& options) {
    kin::EcsWorld world;
    register_components(world);
    populate_world(world, options);
    register_work_system<WorkA>(world, "bench.work_a", "WorkA", 0.11f, options.iterations);
    register_work_system<WorkB>(world, "bench.work_b", "WorkB", 0.23f, options.iterations);
    register_work_system<WorkC>(world, "bench.work_c", "WorkC", 0.37f, options.iterations);
    register_work_system<WorkD>(world, "bench.work_d", "WorkD", 0.51f, options.iterations);
    return world;
}

kin::f64 checksum(kin::EcsWorld& world) {
    kin::f64 sum = 0.0;
    world.query<WorkA>().each([&](const WorkA& work) { sum += work.value; });
    world.query<WorkB>().each([&](const WorkB& work) { sum += work.value; });
    world.query<WorkC>().each([&](const WorkC& work) { sum += work.value; });
    world.query<WorkD>().each([&](const WorkD& work) { sum += work.value; });
    return sum;
}

kin::f64 checksum(const TaskState& state) {
    kin::f64 sum = 0.0;
    const auto add = [&](const std::vector<kin::f32>& values) {
        for (kin::f32 value : values) {
            sum += value;
        }
    };
    add(state.a);
    add(state.b);
    add(state.c);
    add(state.d);
    return sum;
}

kin::f64 run_timed(kin::EcsWorld& world,
                   kin::i32 frames,
                   kin::SystemExecutionMode mode) {
    const auto start = std::chrono::steady_clock::now();
    for (kin::i32 frame = 0; frame < frames; ++frame) {
        if (!world.run_frame(1.0f / 60.0f, mode)) {
            std::cerr << "run_frame failed: " << world.systems().last_error() << "\n";
            std::exit(2);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return static_cast<kin::f64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / 1'000'000.0;
}

std::shared_ptr<TaskState> make_task_state(const Options& options) {
    auto state = std::make_shared<TaskState>();
    state->iterations = options.iterations;
    state->a.reserve(static_cast<std::size_t>(options.entities));
    state->b.reserve(static_cast<std::size_t>(options.entities));
    state->c.reserve(static_cast<std::size_t>(options.entities));
    state->d.reserve(static_cast<std::size_t>(options.entities));
    for (kin::i32 i = 0; i < options.entities; ++i) {
        const kin::f32 base = static_cast<kin::f32>(i) * 0.01f;
        state->a.push_back(base + 1.0f);
        state->b.push_back(base + 2.0f);
        state->c.push_back(base + 3.0f);
        state->d.push_back(base + 4.0f);
    }
    return state;
}

void run_vector_work(std::vector<kin::f32>& values, kin::f32 seed, kin::i32 iterations) {
    for (kin::f32& value : values) {
        value = expensive_step(value, seed, iterations);
    }
}

void register_task_systems(kin::EcsWorld& world, std::shared_ptr<TaskState> state) {
    world.components().data("TaskA");
    world.components().data("TaskB");
    world.components().data("TaskC");
    world.components().data("TaskD");

    world.systems().register_task({
        .id = "bench.task_a",
        .writes = {"TaskA"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [state](kin::SystemContext&) { run_vector_work(state->a, 0.11f, state->iterations); });
    world.systems().register_task({
        .id = "bench.task_b",
        .writes = {"TaskB"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [state](kin::SystemContext&) { run_vector_work(state->b, 0.23f, state->iterations); });
    world.systems().register_task({
        .id = "bench.task_c",
        .writes = {"TaskC"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [state](kin::SystemContext&) { run_vector_work(state->c, 0.37f, state->iterations); });
    world.systems().register_task({
        .id = "bench.task_d",
        .writes = {"TaskD"},
        .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
    }, [state](kin::SystemContext&) { run_vector_work(state->d, 0.51f, state->iterations); });
}

BenchResult run_native_bench(const Options& options) {
    kin::EcsWorld serial = make_world(options);
    kin::EcsWorld parallel = make_world(options);

    for (kin::i32 i = 0; i < options.warmup; ++i) {
        serial.run_frame(1.0f / 60.0f, kin::SystemExecutionMode::SerialGraph);
        parallel.run_frame(1.0f / 60.0f, kin::SystemExecutionMode::ParallelBatches);
    }

    BenchResult result;
    result.serial_ms = run_timed(serial, options.frames, kin::SystemExecutionMode::SerialGraph);
    result.parallel_ms = run_timed(parallel, options.frames, kin::SystemExecutionMode::ParallelBatches);
    result.speedup = result.parallel_ms > 0.0 ? result.serial_ms / result.parallel_ms : 0.0;
    result.checksum = checksum(parallel);

    const kin::SystemScheduleSnapshot& schedule = parallel.systems().schedule_snapshot();
    for (const kin::SystemBatch& batch : schedule.batches) {
        if (batch.execution_mode == kin::SystemExecutionMode::ParallelBatches) {
            ++result.parallel_batches;
        }
    }
    for (const kin::SystemSnapshot& system : parallel.systems().snapshots()) {
        if (system.kind == kin::SystemKind::Native &&
            system.last_execution_mode == kin::SystemExecutionMode::ParallelBatches) {
            ++result.native_parallel_systems;
        }
        result.max_worker_count = std::max(result.max_worker_count, system.worker_count);
        if (system.last_execution_decision == kin::SystemExecutionDecision::Serial) {
            ++result.serial_decisions;
        } else if (system.last_execution_decision == kin::SystemExecutionDecision::ParallelAcrossSystems) {
            ++result.parallel_across_systems_decisions;
        } else if (system.last_execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem) {
            ++result.parallel_within_native_system_decisions;
        }
    }
    return result;
}

BenchResult run_task_bench(const Options& options) {
    kin::EcsWorld serial;
    kin::EcsWorld parallel;
    std::shared_ptr<TaskState> serial_state = make_task_state(options);
    std::shared_ptr<TaskState> parallel_state = make_task_state(options);
    register_task_systems(serial, serial_state);
    register_task_systems(parallel, parallel_state);

    for (kin::i32 i = 0; i < options.warmup; ++i) {
        serial.run_frame(1.0f / 60.0f, kin::SystemExecutionMode::SerialGraph);
        parallel.run_frame(1.0f / 60.0f, kin::SystemExecutionMode::ParallelBatches);
    }

    BenchResult result;
    result.serial_ms = run_timed(serial, options.frames, kin::SystemExecutionMode::SerialGraph);
    result.parallel_ms = run_timed(parallel, options.frames, kin::SystemExecutionMode::ParallelBatches);
    result.speedup = result.parallel_ms > 0.0 ? result.serial_ms / result.parallel_ms : 0.0;
    result.checksum = checksum(*parallel_state);

    const kin::SystemScheduleSnapshot& schedule = parallel.systems().schedule_snapshot();
    for (const kin::SystemBatch& batch : schedule.batches) {
        if (batch.execution_mode == kin::SystemExecutionMode::ParallelBatches) {
            ++result.parallel_batches;
        }
        result.max_worker_count = std::max(result.max_worker_count, batch.worker_count);
        if (batch.execution_decision == kin::SystemExecutionDecision::Serial) {
            ++result.serial_decisions;
        } else if (batch.execution_decision == kin::SystemExecutionDecision::ParallelAcrossSystems) {
            ++result.parallel_across_systems_decisions;
        } else if (batch.execution_decision == kin::SystemExecutionDecision::ParallelWithinNativeSystem) {
            ++result.parallel_within_native_system_decisions;
        }
    }
    return result;
}

Report run_bench(const Options& options) {
    return {
        .task = run_task_bench(options),
        .native = run_native_bench(options),
    };
}

void write_bench_result(kin::JsonWriter& json, const BenchResult& result) {
    json.begin_object();
    json.field("serial_ms", result.serial_ms);
    json.field("parallel_ms", result.parallel_ms);
    json.field("speedup", result.speedup);
    json.field("checksum", result.checksum);
    json.field("parallel_batches", result.parallel_batches);
    json.field("native_parallel_systems", result.native_parallel_systems);
    json.field("serial_decisions", result.serial_decisions);
    json.field("parallel_across_systems_decisions", result.parallel_across_systems_decisions);
    json.field("parallel_within_native_system_decisions", result.parallel_within_native_system_decisions);
    json.field("max_worker_count", result.max_worker_count);
    json.end_object();
}

void write_report(const Options& options, const Report& report) {
    std::ostringstream out;
    kin::JsonWriter json(out);
    json.begin_object();
    json.key("options").begin_object();
    json.field("frames", options.frames);
    json.field("warmup", options.warmup);
    json.field("entities_per_component", options.entities);
    json.field("iterations_per_entity", options.iterations);
    json.end_object();
    json.key("task_systems");
    write_bench_result(json, report.task);
    json.key("native_flecs_systems");
    write_bench_result(json, report.native);
    json.end_object();
    std::cout << out.str() << "\n";
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    const Report report = run_bench(options);
    write_report(options, report);

    if (report.task.parallel_batches <= 0 ||
        report.task.parallel_across_systems_decisions <= 0) {
        std::cerr << "parallel ECS task systems did not execute in parallel mode\n";
        return 3;
    }
    if (options.min_speedup > 0.0 && report.task.speedup < options.min_speedup) {
        std::cerr << "task speedup " << report.task.speedup << " is below required " << options.min_speedup << "\n";
        return 4;
    }
    return 0;
}
