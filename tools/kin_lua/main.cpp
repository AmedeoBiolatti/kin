// kin_lua: Lua tooling for kin projects (docs/lua_development.md).
//   api      the bindings a host offers, as JSON, for tools/kin_lua.py
//   check    whether files are Lua that reads
//   test     isolated test cases (test(name, fn)) against a fresh world each
//   scenario a gameplay scenario (scenario{...}) as the JSON tools/automation runs

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/rng.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/component_schema.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/lua_values.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/scripting/script_reload.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {

using namespace kin;
namespace fs = std::filesystem;

void profile(JsonWriter& json, const LuaRuntimeStats& stats) {
    json.begin_object()
        .field("memory_bytes", stats.memory_bytes)
        .field("peak_memory_bytes", stats.peak_memory_bytes)
        .field("allocated_bytes", stats.allocated_bytes)
        .field("allocations", stats.allocations)
        .field("denied_allocations", stats.denied_allocations)
        .field("gc_steps", stats.gc_steps)
        .field("gc_ns", stats.gc_ns)
        .key("callbacks")
        .begin_array();
    for (const auto& p : stats.callbacks)
        json.begin_object()
            .field("name", p.name)
            .field("calls", p.calls)
            .field("failures", p.failures)
            .field("total_ns", p.total_ns)
            .field("max_ns", p.max_ns)
            .field("allocated_bytes", p.allocated_bytes)
            .end_object();
    json.end_array().end_object();
}
struct Fixture {
    fs::path root;
    kin::EcsWorld world;
    Input input;
    std::unique_ptr<EntityScripts> scripts;
    std::unique_ptr<ScriptEngine> scene;
    RngKey random = make_key(1);
    f64 time = 0;
    u64 frame = 0;
    i32 next_input_key = 1;
    LuaRuntimeLimits limits;
    std::set<std::string> acknowledged;
    std::function<bool(f32)> on_step;
    std::string reload_error;
    explicit Fixture(const fs::path& source, LuaRuntimeLimits limits) : limits(limits) {
        static u64 serial = 0;
        root = fs::temp_directory_path() /
               ("kin-lua-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++serial));
        fs::create_directories(root);
        try {
            set_user_data_dir_override(root / ".user-data");
            set_user_data_game_id("kin_lua_test"); // input profiles and saves go to the case's own folder
            u64 bytes = 0;
            auto files = list_content_files(source, true);
            if (files.size() > 4096)
                throw std::runtime_error("test content exceeds 4096 files");
            for (const auto& file : files) {
                auto data = read_content_file(file);
                if (!data || (bytes += data->size()) > 64 * 1024 * 1024)
                    throw std::runtime_error("test content exceeds 64 MiB or cannot be read");
                auto target = path(file.lexically_relative(source).generic_string());
                fs::create_directories(target.parent_path());
                std::ofstream out(target, std::ios::binary);
                out << *data;
                if (!out)
                    throw std::runtime_error("could not copy test content");
            }
            register_builtin_editor_components(world);
            scripts = std::make_unique<EntityScripts>(
                EntityScriptsConfig{.content = root, .check_frames = 0, .runtime_limits = limits});
        } catch (...) {
            set_user_data_game_id({});
            clear_user_data_dir_override();
            fs::remove_all(root);
            throw;
        }
    }
    ~Fixture() {
        scene.reset();
        scripts.reset();
        set_user_data_game_id({});
        clear_user_data_dir_override();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path(std::string_view name) const {
        const fs::path relative(name);
        if (relative.empty() || relative.has_root_path())
            throw std::runtime_error("fixture paths must be relative");
        for (const auto& part : relative)
            if (part == "..")
                throw std::runtime_error("fixture paths cannot escape content root");
        return root / relative;
    }
    kin::EcsEntity entity(u64 id) {
        if (!world.raw().is_alive(id))
            throw std::runtime_error("stale test entity");
        return kin::EcsEntity{world.raw().entity(id)};
    }
    void patch(u64 id, const std::string& component, sol::table fields) {
        auto e = entity(id);
        auto& registry = world.components();
        if (!registry.has(e, component) && !registry.add(e, component))
            throw std::runtime_error("unknown component: " + component);
        std::vector<std::string> errors;
        const std::optional<JsonValue> json = lua_to_json(sol::make_object(fields.lua_state(), fields));
        if (!json || !json->is_object()) {
            throw std::runtime_error(component + ": test component values must be a table of data");
        }
        auto values = parse_component_fields(component, *json, registry, errors);
        if (!errors.empty())
            throw std::runtime_error(errors.front());
        for (const auto& field : values)
            if (!registry.patch_field(e, component, field.name, field.value))
                throw std::runtime_error(registry.last_error());
    }
    void bind(sol::state& lua) {
        sol::table t = lua.create_named_table("t");
        t["spawn"] = [this](std::string name, sol::optional<sol::table> components) {
            auto e = world.entity(name);
            if (components)
                for (const auto& [key, value] : *components)
                    patch(e.id(), key.as<std::string>(), value.as<sol::table>());
            return e.id();
        };
        t["get"] = [this](u64 id, std::string component, sol::this_state state) -> sol::object {
            sol::state_view lua{state};
            sol::table fields = lua.create_table();
            const bool found = world.components().visit_fields(
                entity(id), component,
                [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
                    fields[std::string(name)] = component_value_to_lua(lua, value);
                });
            return found ? sol::make_object(lua, fields) : sol::make_object(lua, sol::nil);
        };
        t["patch"] = [this](u64 id, std::string component, sol::table fields) { patch(id, component, fields); };
        t["destroy"] = [this](u64 id) { entity(id).raw().destruct(); };
        t["alive"] = [this](u64 id) { return world.raw().is_alive(id); };
        t["attach"] = [this](u64 id, std::string file) {
            path(file);
            entity(id).set(ScriptComponent{.asset = {.id = std::move(file)}});
        };
        t["enabled"] = [this](u64 id, bool enabled) { entity(id).raw().enable(enabled); };
        t["input"] = [this](std::string action, bool held) {
            if (!input.map().bindings(action)) {
                if (next_input_key > static_cast<i32>(LastKey))
                    throw std::runtime_error("too many test input actions");
                auto map = input.map();
                map.bind(action, static_cast<Key>(next_input_key++));
                input.set_map(map);
            }
            input.set_action_held(action, held);
        };
        t["step"] = [this](sol::optional<i32> frames, sol::optional<f32> dt) {
            const auto count = frames.value_or(1);
            const auto delta = dt.value_or(1.0f / 60.0f);
            if (count < 1 || count > 100000 || !std::isfinite(delta) || delta < 0)
                throw std::runtime_error("invalid test step");
            for (i32 i = 0; i < count; ++i) {
                if (scene && !scene->call_update(ScriptFrameContext{delta, &input}))
                    throw std::runtime_error(scene->last_error());
                scripts->update(world, input, delta);
                if (!world.systems().run_frame(delta))
                    throw std::runtime_error("Lua test system failed");
                if (on_step && !on_step(delta))
                    throw std::runtime_error("Lua test task failed");
                input.advance_step_edges();
                input.begin_frame();
                time += delta;
                ++frame;
            }
        };
        t["time"] = [this] { return time; };
        t["frame"] = [this] { return frame; };
        t["seed"] = [this](u64 seed) { random = make_key(seed); };
        t["random"] = [this] {
            auto keys = split(random);
            random = keys.first;
            return rng_f32(keys.second, 0, 1);
        };
        t["errors"] = [this](sol::this_state state) {
            auto out = sol::state_view(state).create_table();
            for (const auto& [path, error] : scripts->problems()) {
                out[path] = error;
                acknowledged.insert(path + "\n" + error);
            }
            return out;
        };
        t["edit"] = [this](std::string file, std::string code) {
            if (code.size() > 4 * 1024 * 1024)
                throw std::runtime_error("test edit exceeds 4 MiB");
            auto target = path(file);
            fs::create_directories(target.parent_path());
            std::ofstream out(target, std::ios::binary);
            out << code;
            if (!out)
                throw std::runtime_error("could not write test edit");
        };
        t["reload"] = [this](std::string file) {
            path(file);
            return scripts->reload(file);
        };
        t["scene"] = [this](std::string file) {
            path(file);
            auto next = std::make_unique<ScriptEngine>(std::function<void(sol::state&)>{}, limits);
            // The entity scripts' world services (one per world) serve the scene script too.
            if (!next->load_file(root, file) ||
                !next->call_on_load(ScriptSceneBindings{.world = &world, .script_path = root / file}))
                throw std::runtime_error(next->last_error());
            scene = std::move(next);
        };
        t["reload_scene"] = [this](std::string file) {
            path(file);
            reload_error.clear();
            if (!scene)
                throw std::runtime_error("no test scene loaded");
            auto next = scene->prepare_reload(root, file, reload_error);
            if (!next)
                return false;
            if (!activate_script(scene, next, reload_error)) {
                rollback_script(scene, next, false);
                return false;
            }
            return true;
        };
        t["reload_error"] = [this] { return reload_error; };
        sol::protected_function finish = lua.script(R"lua(
            local function same(a,b,depth)
                if type(a)~=type(b) then return false end
                if type(a)~='table' then return a==b end
                assert(depth<32,'assertion table exceeds nesting limit')
                for k,v in pairs(a) do if not same(v,b[k],depth+1) then return false end end
                for k in pairs(b) do if a[k]==nil then return false end end
                return true
            end
            function t.equal(actual,expected,message)
                assert(same(actual,expected,0),message or ('expected '..tostring(expected)..', got '..tostring(actual)))
            end
            function t.near(actual,expected,epsilon)
                assert(type(actual)=='number' and type(expected)=='number' and math.abs(actual-expected)<=(epsilon or 0.00001),'values differ')
            end
            function t.raises(fn,pattern)
                local ok,err=pcall(fn)
                assert(not ok,'expected an error')
                assert(not pattern or tostring(err):find(pattern,1,true),'unexpected error: '..tostring(err))
            end
            local run, jobs = task.run, {}
            task.run = function(fn)
                local job = run(fn)
                jobs[#jobs+1] = job
                return job
            end
            return function()
                for _,job in ipairs(jobs) do
                    local state = job:state()
                    assert(state=='completed' or state=='cancelled' or state=='failed',
                        'test left an unfinished task; step the simulation or cancel it')
                end
            end
        )lua");
        lua.registry()["kin.tests.finish"] = finish;
    }
};
struct CaseResult {
    std::string file, name, error;
    LuaRuntimeStats stats;
    std::map<std::string, LuaRuntimeStats> hosts;
};
int tests(const fs::path& root, const std::vector<fs::path>& files, const fs::path& report, const fs::path& junit,
          LuaRuntimeLimits limits) {
    std::vector<CaseResult> results;
    for (const auto& file : files) {
        std::vector<std::string> names;
        {
            LuaScript registration{{.setup =
                                        [&](sol::state& lua) {
                                            lua["test"] = [&](std::string name, sol::protected_function) {
                                                if (name.empty() || std::ranges::find(names, name) != names.end())
                                                    throw std::runtime_error("test names must be nonempty and unique");
                                                if (names.size() >= 4096)
                                                    throw std::runtime_error("test file exceeds 4096 cases");
                                                names.push_back(std::move(name));
                                            };
                                        },
                                    .module_root = root,
                                    .limits = limits,
                                    .on_error = [](std::string_view) {}}};
            if (!registration.load_file(file)) {
                results.push_back({file.string(), "<load>", registration.error(), registration.runtime().stats()});
                continue;
            }
        }
        if (names.empty()) {
            results.push_back({file.string(), "<discovery>", "no tests registered", {}});
            continue;
        }
        for (const auto& name : names) {
            CaseResult result{file.string(), name, {}, {}};
            try {
                Fixture fixture(root, limits);
                // All Lua references are scoped below the VM that owns them.
                LuaScript script{{.setup =
                                      [&](sol::state& lua) {
                                          fixture.bind(lua);
                                          lua["test"] = [&](std::string found, sol::protected_function fn,
                                                            sol::this_state state) {
                                              if (found == name)
                                                  sol::state_view{state}["__kin_test"] = std::move(fn);
                                          };
                                      },
                                  .module_root = fixture.root,
                                  .limits = limits,
                                  .on_error = [](std::string_view) {}}};
                if (!script.load_file(file))
                    throw std::runtime_error(script.error());
                sol::protected_function target = script.lua()["__kin_test"];
                if (!target.valid())
                    throw std::runtime_error("test registration changed during isolated run");
                fixture.on_step = [&](f32 dt) { return script.advance_tasks(dt); };
                const auto run = [&](sol::protected_function function, std::string_view call_name) {
                    LuaCall call(script.runtime(), script.lua().lua_state(), call_name);
                    const sol::protected_function_result called = call.invoke(function);
                    if (!called.valid()) {
                        const sol::error error = called;
                        result.error = error.what();
                    }
                };
                run(target, name);
                if (result.error.empty()) {
                    run(script.lua().registry()["kin.tests.finish"], "test.finish");
                }
                result.stats = script.runtime().stats();
                result.hosts = fixture.scripts->runtime_stats();
                if (fixture.scene)
                    result.hosts["<scene>"] = fixture.scene->runtime_stats();
                if (result.error.empty())
                    for (const auto& [path, error] : fixture.scripts->problems())
                        if (!fixture.acknowledged.contains(path + "\n" + error)) {
                            result.error = error;
                            break;
                        }
            } catch (const std::exception& e) {
                result.error = e.what();
            }
            std::fprintf(stderr, "%s %s: %s\n", result.error.empty() ? "PASS" : "FAIL", result.name.c_str(),
                         result.error.c_str());
            results.push_back(std::move(result));
        }
    }
    const auto failures = std::ranges::count_if(results, [](const auto& r) { return !r.error.empty(); });
    auto write = [&](std::ostream& out) {
        JsonWriter json(out);
        json.begin_object()
            .field("schema", "kin.lua-tests/1")
            .field("tests", static_cast<i64>(results.size()))
            .field("failures", static_cast<i64>(failures))
            .key("cases")
            .begin_array();
        for (const auto& r : results) {
            json.begin_object()
                .field("file", r.file)
                .field("name", r.name)
                .field("passed", r.error.empty())
                .field("error", r.error)
                .key("profile");
            profile(json, r.stats);
            json.key("hosts").begin_object();
            for (const auto& [name, stats] : r.hosts) {
                json.key(name);
                profile(json, stats);
            }
            json.end_object().end_object();
        }
        json.end_array().end_object();
        out << '\n';
    };
    if (report.empty())
        write(std::cout);
    else {
        std::ofstream out(report);
        if (!out)
            throw std::runtime_error("cannot write test report");
        write(out);
    }
    if (!junit.empty()) {
        auto xml = [](std::string_view text) {
            std::string out;
            for (char c : text) {
                switch (c) {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                case '"':
                    out += "&quot;";
                    break;
                default:
                    out += c;
                }
            }
            return out;
        };
        std::ofstream out(junit);
        if (!out)
            throw std::runtime_error("cannot write JUnit report");
        out << "<testsuite name=\"kin.lua\" tests=\"" << results.size() << "\" failures=\"" << failures << "\">\n";
        for (const auto& r : results) {
            out << "<testcase classname=\"" << xml(r.file) << "\" name=\"" << xml(r.name) << "\">";
            if (!r.error.empty())
                out << "<failure message=\"" << xml(r.error) << "\"/>";
            out << "</testcase>\n";
        }
        out << "</testsuite>\n";
    }
    return failures ? 1 : 0;
}
// A scenario's value as tools/automation reads it (kin.scenario/1): strictly
// data, so a mistake fails the compile instead of changing the scenario.
JsonValue scenario_value(const sol::object& object, int depth = 0) {
    if (depth > 32) {
        throw sol::error("scenario table nesting exceeds 32");
    }
    switch (object.get_type()) {
    case sol::type::boolean:
        return JsonValue{object.as<bool>()};
    case sol::type::number: {
        const auto number = object.as<double>();
        if (!std::isfinite(number)) {
            throw sol::error("non-finite scenario number");
        }
        return JsonValue{number};
    }
    case sol::type::string:
        return JsonValue{object.as<std::string>()};
    case sol::type::nil:
        return JsonValue{};
    case sol::type::table: {
        JsonValue::Object fields;
        std::map<std::size_t, JsonValue> entries;
        for (const auto& [key, item] : object.as<sol::table>()) {
            if (key.get_type() == sol::type::string) {
                fields.emplace(key.as<std::string>(), scenario_value(item, depth + 1));
            } else if (key.get_type() == sol::type::number) {
                const double index = key.as<double>();
                if (index < 1 || index > 10000 || std::floor(index) != index) {
                    throw sol::error("invalid scenario array index");
                }
                entries.emplace(static_cast<std::size_t>(index), scenario_value(item, depth + 1));
            } else {
                throw sol::error("scenario keys must be strings or array indices");
            }
            if (fields.size() + entries.size() > 10000) {
                throw sol::error("scenario table exceeds 10000 items");
            }
        }
        if (!fields.empty() && !entries.empty()) {
            throw sol::error("mixed scenario object/array");
        }
        if (!entries.empty()) {
            JsonValue::Array array;
            for (auto& [index, item] : entries) {
                if (index != array.size() + 1) {
                    throw sol::error("scenario array has holes");
                }
                array.push_back(std::move(item));
            }
            return JsonValue{std::move(array)};
        }
        return JsonValue{std::move(fields)};
    }
    default:
        throw sol::error("scenario values must be JSON-compatible");
    }
}

// The step builders a scenario file uses, besides scenario{...} itself.
constexpr std::pair<const char*, const char*> scenario_conditions[] = {
    {"expect", "expect"}, {"wait_until", "wait"}, {"always", "always"}};

void bind_scenario(sol::state& lua, std::optional<JsonValue>& document) {
    lua["scenario"] = [&document](sol::table table) {
        if (document) {
            throw sol::error("one scenario per file");
        }
        table["schema"] = "kin.scenario/1";
        document = scenario_value(sol::object(table));
    };
    lua["call"] = [](const std::string& method, sol::optional<sol::table> params, sol::this_state state) {
        auto out = sol::state_view(state).create_table_with("call", method);
        if (params) {
            out["params"] = *params;
        }
        return out;
    };
    for (const auto& [name, kind] : scenario_conditions) {
        lua[name] = [kind](sol::table condition, sol::this_state state) {
            return sol::state_view(state).create_table_with(kind, condition);
        };
    }
    lua["tick"] = [](int count, sol::this_state state) {
        return sol::state_view(state).create_table_with("tick", count);
    };
    lua["capture"] = [](const std::string& name, sol::this_state state) {
        return sol::state_view(state).create_table_with("capture", name);
    };
}

// Compiles a scenario file to the JSON tools/automation's runner takes.
int scenario(const fs::path& file) {
    std::optional<JsonValue> document;
    LuaScript script{{.setup = [&](sol::state& lua) { bind_scenario(lua, document); },
                      .module_root = file.parent_path(),
                      .limits = {.instruction_limit = 500'000},
                      .on_error = [](std::string_view) {}}};
    if (!script.load_file(file)) {
        throw std::runtime_error(script.error());
    }
    if (!document) {
        throw std::runtime_error("a scenario file must call scenario{...}");
    }
    JsonWriter writer(std::cout, false);
    write_json(writer, *document);
    std::cout << '\n';
    return 0;
}

int api(std::string_view host, const std::vector<fs::path>& schemas) {
    LuaApiSurface surface;
    std::vector<std::string> functions; // the global functions the host adds
    if (host == "scene") {
        ScriptEngine script;
        surface = script.api_surface();
    } else if (host == "entity") {
        EntityScripts script{{.content = {}}};
        surface = script.api_surface();
    } else if (host == "test") {
        auto content = MemoryContent::create({});
        Fixture fixture(content->root(), {});
        LuaScript script{{.setup = [&](sol::state& lua) { fixture.bind(lua); }}};
        surface = script.api_surface();
        functions = {"test"};
    } else if (host == "scenario") {
        std::optional<JsonValue> document;
        LuaScript script{{.setup = [&](sol::state& lua) { bind_scenario(lua, document); }}};
        surface = script.api_surface();
        functions = {"scenario", "call", "tick", "capture"};
        for (const auto& [name, kind] : scenario_conditions) {
            functions.emplace_back(name);
        }
        std::ranges::sort(functions);
    } else {
        throw std::runtime_error("API host must be scene, entity, test or scenario");
    }
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    for (const auto& path : schemas) {
        auto loaded = load_component_schema_asset(path);
        std::vector<std::string> errors;
        if (!loaded.ok())
            throw std::runtime_error(loaded.diagnostics.empty() ? "could not load component schema"
                                                                : loaded.diagnostics.front());
        if (!apply_component_schema_asset(world.components(), *loaded.document, errors))
            throw std::runtime_error(errors.empty() ? "invalid component schema" : errors.front());
    }
    const auto lists = [](JsonWriter& json, const std::map<std::string, std::vector<std::string>>& entries) {
        json.begin_object();
        for (const auto& [name, items] : entries) {
            json.key(name).begin_array();
            for (const auto& item : items) {
                json.value(item);
            }
            json.end_array();
        }
        json.end_object();
    };
    JsonWriter json(std::cout);
    json.begin_object().field("schema", "kin.lua-api/1").field("host", host).key("tables");
    lists(json, surface.tables);
    json.key("properties");
    lists(json, surface.properties);
    json.key("callbacks");
    lists(json, surface.callbacks);
    json.key("functions").begin_array();
    for (const auto& name : functions) {
        json.value(name);
    }
    json.end_array().key("component_aliases").begin_object();
    for (const auto& [alias, name] : surface.component_aliases) {
        json.field(alias, name);
    }
    json.end_object().key("component_shapes").begin_object();
    for (const auto& [name, fields] : surface.component_fields) {
        json.key(name).begin_object();
        for (const auto& [field, kind] : fields) {
            json.field(field, kind);
        }
        json.end_object();
    }
    json.end_object().key("components").begin_object();
    for (const auto& component : world.components().descriptors()) {
        json.key(component.name).begin_object();
        for (const auto& f : component.fields)
            json.key(f.name)
                .begin_object()
                .field("kind", component_field_kind_name(f.kind))
                .field("readonly", f.readonly)
                .field("description", f.description)
                .end_object();
        json.end_object();
    }
    json.end_object().end_object();
    std::cout << '\n';
    return 0;
}

} // namespace
int main(int argc, char** argv) {
    try {
        if (argc < 2)
            throw std::runtime_error("usage: kin_lua api [--host scene|entity|test|scenario] | check file.lua... | "
                                     "test [--root content] [--json report.json] [--junit report.xml] files... | "
                                     "scenario file.lua");
        const std::string command = argv[1];
        fs::path root = fs::current_path(), report, junit;
        std::string host = "scene";
        std::vector<fs::path> files, schemas;
        LuaRuntimeLimits limits;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&] {
                if (++i >= argc)
                    throw std::runtime_error("missing argument for " + arg);
                return std::string(argv[i]);
            };
            if (arg == "--root")
                root = fs::absolute(next());
            else if (arg == "--json")
                report = next();
            else if (arg == "--junit")
                junit = next();
            else if (arg == "--host")
                host = next();
            else if (arg == "--components")
                schemas.emplace_back(next());
            else if (arg == "--instructions")
                limits.instruction_limit = std::stoull(next());
            else if (arg == "--memory")
                limits.memory_limit_bytes = std::stoull(next());
            else if (arg.starts_with("--"))
                throw std::runtime_error("unknown option: " + arg);
            else
                files.emplace_back(fs::absolute(arg));
        }
        if (command == "api")
            return api(host, schemas);
        if (files.empty())
            throw std::runtime_error("no Lua files supplied");
        if (command == "test")
            return tests(root, files, report, junit, limits);
        if (command == "scenario") {
            if (files.size() != 1)
                throw std::runtime_error("kin_lua scenario compiles one file");
            return scenario(files.front());
        }
        if (command == "check") {
            bool ok = true;
            for (const auto& path : files) {
                auto code = read_content_text(path);
                std::string error;
                if (!code || !check_lua_syntax(*code, path.string(), error)) {
                    std::fprintf(stderr, "%s\n", code ? error.c_str() : ("cannot read " + path.string()).c_str());
                    ok = false;
                }
            }
            return ok ? 0 : 1;
        }
        throw std::runtime_error("unknown command: " + command);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kin_lua: %s\n", e.what());
        return 2;
    }
}
