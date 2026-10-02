#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/runtime/state_hash.hpp>

#include <cassert>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// This executable is also the "game" the determinism check runs: started with
// --state-lockstep, it plays the scenario named by --scenario=.

namespace {

struct Mover {
    kin::Vec2f pos{};
    kin::Vec2f velocity{};
};

struct Stamp {
    kin::i64 value = 0;
};

struct Label {
    std::string text;
};

struct Frozen {};

// Moves a few entities each update; the scenario decides what else happens.
// Its update N is the run's frame N + 1: on frame 1 the scene is only pushed.
class ScenarioScene final : public kin::Scene {
public:
    explicit ScenarioScene(std::string scenario)
        : _scenario(std::move(scenario)) {
        _world.component<Mover>("Mover");
        _world.component<Stamp>("Stamp");
        _world.component<Label>("Label");
        _world.component<Frozen>("Frozen");
        for (int i = 0; i < 4; ++i) {
            _world.entity("mover_" + std::to_string(i))
                .set(Mover{.pos = {static_cast<kin::f32>(i), 0.0f}, .velocity = {1.0f, 0.5f * static_cast<kin::f32>(i)}});
        }
        _world.entity("stamp").set(Stamp{});
        _world.entity("label").set(Label{.text = "a string owns heap memory"});
    }

    std::string_view name() const override { return "Scenario"; }
    kin::EcsWorld* world() override { return &_world; }

    void update(kin::SceneContext&) override {
        ++_frame;
        _world.raw().each([](Mover& mover) {
            mover.pos.x += mover.velocity.x;
            mover.pos.y += mover.velocity.y;
        });
        if (_scenario == "clock" && _frame == 5) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            _world.lookup("stamp").set(Stamp{.value = static_cast<kin::i64>(now)});
        }
        if (_scenario == "workers" && _frame == 3) {
            _world.lookup("stamp").set(Stamp{.value = kin::default_job_system().worker_count()});
        }
        if (_scenario == "spawn" && _frame == 4 && kin::default_job_system().worker_count() == 1) {
            _world.entity("only_with_one_worker").set(Mover{});
        }
        if (_scenario == "tag" && _frame == 6 && kin::default_job_system().worker_count() == 1) {
            _world.lookup("mover_2").add<Frozen>();
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("frame", _frame);
        if (_scenario == "report" && _frame >= 7) {
            json.field("elapsed_ns", static_cast<kin::i64>(std::chrono::steady_clock::now().time_since_epoch().count()));
        }
    }

private:
    std::string _scenario;
    kin::EcsWorld _world;
    kin::i32 _frame = 0;
};

std::string scenario_of(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--scenario=")) {
            return std::string{arg.substr(11)};
        }
    }
    return "steady";
}

kin::JsonValue check(const char* program, std::string_view scenario, kin::i32 frames, int& code) {
    kin::SceneManager unused;
    std::ostringstream out;
    kin::HeadlessOptions options;
    options.check_determinism = true;
    options.frames = frames;
    options.seed = 3;
    options.args = {program, "--scenario=" + std::string{scenario}};
    code = kin::run_scene_app({
        .window = {.title = "determinism", .width = 32, .height = 32},
        .headless = options,
        .determinism_output = &out,
    }, unused);
    const kin::JsonParseResult parsed = kin::parse_json(out.str());
    assert(parsed.ok());
    return *parsed.value;
}

const kin::JsonValue& run_named(const kin::JsonValue& report, std::string_view name) {
    for (const kin::JsonValue& run : report.members().at("runs").items()) {
        if (run.string_at("name") == name) {
            return run;
        }
    }
    assert(false && "no such run");
    return report;
}

const kin::JsonValue& first_difference(const kin::JsonValue& run) {
    return run.members().at("differences").items().front();
}

// Same state, same hash; any change to a value, a tag, an entity, or the report
// changes it; components that own memory are left out and listed.
void test_state_hash() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "state", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    const auto make = [&](std::string scenario) {
        auto scenes = std::make_unique<kin::SceneManager>();
        scenes->push(std::make_unique<ScenarioScene>(std::move(scenario)));
        kin::SceneContext ctx{.app = app, .window = window, .renderer = renderer, .input = app.input(), .scenes = *scenes};
        scenes->flush_pending(ctx);
        return scenes;
    };
    auto a = make("steady");
    auto b = make("steady");
    kin::StateCoverage coverage;
    const kin::u64 hash = kin::hash_state(*a, &coverage);
    assert(hash == kin::hash_state(*b));
    assert(coverage.scenes == 1);
    assert(coverage.entities >= 6); // the scenario's, and the world's system phases
    assert(coverage.values == 5); // four Movers and the Stamp; the Label is not compared
    assert(coverage.not_compared.size() == 2);
    assert(coverage.not_compared[0] == "(Identifier,Name)" && coverage.not_compared[1] == "Label");

    kin::EcsWorld& world = *b->at_mut(0)->world();
    world.lookup("mover_1").set(Mover{.pos = {1.0f, 0.0f}, .velocity = {1.0f, 0.5000001f}});
    assert(kin::hash_state(*b) != hash);
    world.lookup("mover_1").set(Mover{.pos = {1.0f, 0.0f}, .velocity = {1.0f, 0.5f}});
    assert(kin::hash_state(*b) == hash);
    world.lookup("mover_1").add<Frozen>();
    assert(kin::hash_state(*b) != hash);
    world.lookup("mover_1").remove<Frozen>();
    assert(kin::hash_state(*b) == hash);
    world.lookup("label").set(Label{.text = "not compared"});
    assert(kin::hash_state(*b) == hash);
    world.entity("new");
    assert(kin::hash_state(*b) != hash);

    const std::vector<kin::StateItem> items = kin::describe_state(*a);
    bool found_mover = false;
    for (const kin::StateItem& item : items) {
        if (item.entity_name == "mover_3" && item.component == "Mover") {
            found_mover = true;
            assert(item.bytes.size() == sizeof(Mover));
            assert(item.scene_name == "Scenario");
        }
        assert(item.component != "Label");
    }
    assert(found_mover);
    assert(items.front().component == "(report)" && items.front().entity == 0);
}

void test_check_passes_when_deterministic(const char* program) {
    int code = -1;
    const kin::JsonValue report = check(program, "steady", 20, code);
    assert(code == 0);
    assert(report.string_at("schema") == "kin.determinism/1");
    assert(report.string_at("status") == "ok");
    assert(report.int_at("frames") == 20);
    for (const kin::JsonValue& run : report.members().at("runs").items()) {
        assert(run.string_at("status") == "ok");
        assert(run.int_at("frames") == 20);
    }
    const kin::JsonValue& coverage = report.members().at("coverage");
    assert(coverage.int_at("scenes") == 1 && coverage.int_at("entities") >= 6 && coverage.int_at("values") == 5);
    assert(coverage.members().at("not_compared").items().size() == 2);
}

// A value from the clock: both runs part from the baseline at that frame, on
// that entity and component.
void test_check_finds_clock(const char* program) {
    int code = -1;
    const kin::JsonValue report = check(program, "clock", 20, code);
    assert(code == 1);
    assert(report.string_at("status") == "diverged");
    for (std::string_view name : {"repeat", "one worker"}) {
        const kin::JsonValue& run = run_named(report, name);
        assert(run.string_at("status") == "diverged");
        assert(run.int_at("first_frame") == 6);
        assert(run.int_at("total_differences") == 1);
        const kin::JsonValue& difference = first_difference(run);
        assert(difference.string_at("name") == "stamp");
        assert(difference.string_at("component") == "Stamp");
        assert(difference.string_at("change") == "value");
        assert(difference.members().at("baseline").string_at("hex").size() == sizeof(Stamp) * 2);
    }
}

// A value that depends on the number of job workers: the repeat agrees, the
// one-worker run does not (unless this machine only gives one worker anyway).
void test_check_finds_worker_dependence(const char* program) {
    int code = -1;
    const kin::JsonValue report = check(program, "workers", 20, code);
    assert(run_named(report, "repeat").string_at("status") == "ok");
    if (kin::default_job_system().worker_count() == 1) {
        return;
    }
    assert(code == 1);
    const kin::JsonValue& run = run_named(report, "one worker");
    assert(run.string_at("status") == "diverged" && run.int_at("first_frame") == 4);
    assert(first_difference(run).string_at("name") == "stamp");
    assert(run_named(report, "one worker").members().at("environment").string_at("KIN_JOB_WORKERS") == "1");
}

// An entity in one run only, and a tag added in one run only.
void test_check_finds_entities_and_tags(const char* program) {
    if (kin::default_job_system().worker_count() == 1) {
        return;
    }
    int code = -1;
    kin::JsonValue report = check(program, "spawn", 20, code);
    const kin::JsonValue& spawn = run_named(report, "one worker");
    assert(spawn.int_at("first_frame") == 5);
    bool only_here = false;
    for (const kin::JsonValue& difference : spawn.members().at("differences").items()) {
        only_here = only_here || (difference.string_at("name") == "only_with_one_worker" &&
                                  difference.string_at("change") == "only in this run");
    }
    assert(only_here);

    report = check(program, "tag", 20, code);
    const kin::JsonValue& tag = run_named(report, "one worker");
    assert(tag.int_at("first_frame") == 7 && tag.int_at("total_differences") == 1);
    assert(first_difference(tag).string_at("name") == "mover_2");
    assert(first_difference(tag).string_at("component") == "(type)");
}

// A nondeterministic report field is named by its path.
void test_check_finds_report_fields(const char* program) {
    int code = -1;
    const kin::JsonValue report = check(program, "report", 20, code);
    assert(code == 1);
    const kin::JsonValue& run = run_named(report, "repeat");
    assert(run.int_at("first_frame") == 8);
    const kin::JsonValue& difference = first_difference(run);
    assert(difference.string_at("component") == "(report)");
    const kin::JsonValue& field = difference.members().at("fields").items().front();
    assert(field.string_at("path") == ".elapsed_ns");
}

void test_options() {
    const char* argv[] = {"game", "--check-determinism=out/det.json", "--frames=50", "--seed=4"};
    const kin::HeadlessOptions options = kin::parse_headless_options(4, const_cast<char**>(argv));
    assert(options.check_determinism && options.determinism_path == "out/det.json");
    assert(options.args.size() == 4 && options.args[0] == "game" && options.args[3] == "--seed=4");
    const char* lockstep[] = {"game", "--state-lockstep"};
    const kin::HeadlessOptions child = kin::parse_headless_options(2, const_cast<char**>(lockstep));
    assert(child.state_lockstep && child.enabled);
}

} // namespace

int main(int argc, char** argv) {
    const kin::HeadlessOptions options = kin::parse_headless_options(argc, argv);
    if (options.state_lockstep) {
        kin::SceneManager scenes;
        scenes.push(std::make_unique<ScenarioScene>(scenario_of(argc, argv)));
        return kin::run_scene_app({.window = {.title = "determinism", .width = 32, .height = 32}, .headless = options},
                                  scenes);
    }

    test_options();
    test_state_hash();
    test_check_passes_when_deterministic(argv[0]);
    test_check_finds_clock(argv[0]);
    test_check_finds_worker_dependence(argv[0]);
    test_check_finds_entities_and_tags(argv[0]);
    test_check_finds_report_fields(argv[0]);
    return 0;
}
