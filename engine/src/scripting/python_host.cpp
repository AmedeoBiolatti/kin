// A PythonScripts (kin/scripting/python.hpp): its interpreter, its modules
// and their reloads, and each update's order: starts, events, systems, tasks.

#include "python_internal.hpp"

#include <kin/ecs/tween.hpp>
#include <kin/platform/log.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include "kin_python_embedded.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <system_error>

namespace kin::python_detail {
namespace {

// PocketPy keeps its interpreters in globals: 16 of them, 0 for
// check_python_syntax and one each for the PythonScripts there are.
struct Runtime {
    bool initialized = false;
    std::array<Host*, 16> hosts{};
};

Runtime& runtime() {
    static Runtime made;
    return made;
}

// Makes `vm` the current interpreter, and the one before it again once done.
class VmScope {
public:
    explicit VmScope(int vm) : _previous(py_currentvm()) {
        if (_previous != vm) {
            py_switchvm(vm);
        }
    }
    ~VmScope() {
        if (py_currentvm() != _previous) {
            py_switchvm(_previous);
        }
    }
    VmScope(const VmScope&) = delete;
    VmScope& operator=(const VmScope&) = delete;

private:
    int _previous;
};

// One call's budget (pocketpy_sandbox.h); a call made from inside another
// (a stop() while a system destroys) runs on the outer one's.
class Budget {
public:
    explicit Budget(Host& host) : _host(host) {
        if (_host.budget_depth++ == 0) {
            kin_pocketpy_budget_begin(_host.config.instruction_limit, _host.config.memory_limit);
        }
    }
    ~Budget() {
        if (--_host.budget_depth == 0) {
            kin_pocketpy_budget_end();
        }
    }
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;

private:
    Host& _host;
};

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : std::optional(time);
}

bool read_text(const std::filesystem::path& file, std::string& text) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad();
}

// "levels.forest" for levels/forest.py; empty for a name a module cannot have.
std::string module_name_of(const std::filesystem::path& relative) {
    std::string name;
    for (const std::filesystem::path& part : relative.parent_path()) {
        name += part.generic_string() + ".";
    }
    name += relative.stem().generic_string();
    const bool valid = !name.empty() && std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    });
    return valid ? name : std::string();
}

std::string file_of_module(std::string_view name) {
    std::string file(name);
    std::ranges::replace(file, '.', '/');
    return file + ".py";
}

// The exception raised, as problems() tells it (file:line: what), and its traceback.
std::pair<std::string, std::string> describe_exception(KinPocketPyStop stop) {
    char* formatted = py_formatexc();
    std::string traceback = formatted ? formatted : "";
    py_free(formatted);
    std::string where;
    std::string what = traceback;
    // Its innermost frame is the last: `  File "name", line N[, in function]`,
    // then that line's source, then `Type: message`. The script's own frame
    // is told, not kin's (kin.py) it was called from.
    std::size_t file = traceback.rfind("File \"");
    for (std::size_t script = file; script != std::string::npos && script > 0;
         script = traceback.rfind("File \"", script - 1)) {
        if (traceback.compare(script, 14, "File \"kin.py\"") != 0) {
            file = script;
            break;
        }
    }
    if (file != std::string::npos) {
        const std::size_t name_end = traceback.find('"', file + 6);
        const std::string_view after =
            std::string_view(traceback).substr(name_end == std::string::npos ? traceback.size() : name_end);
        if (after.starts_with("\", line ")) {
            const std::size_t digits = name_end + 8;
            std::size_t end = digits;
            while (end < traceback.size() && std::isdigit(static_cast<unsigned char>(traceback[end]))) {
                ++end;
            }
            where = traceback.substr(file + 6, name_end - file - 6) + ":" + traceback.substr(digits, end - digits) + ": ";
        }
        // What it raised: the traceback's last line.
        std::string_view rest(traceback);
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == ' ')) {
            rest.remove_suffix(1);
        }
        const std::size_t last = rest.rfind('\n');
        what = std::string(last == std::string_view::npos ? rest : rest.substr(last + 1));
    } else if (const std::size_t last = traceback.rfind('\n'); last != std::string::npos) {
        what = traceback.substr(last + 1);
    }
    while (!what.empty() && (what.back() == '\n' || what.back() == ' ')) {
        what.pop_back();
    }
    if (stop == KIN_POCKETPY_OUT_OF_INSTRUCTIONS) {
        what = "instruction limit reached";
    } else if (stop == KIN_POCKETPY_OUT_OF_MEMORY) {
        what = "memory limit reached";
    }
    return {where + what, traceback};
}

std::string name_of_module(py_Ref module) {
    if (!module) {
        return {};
    }
    const py_ItemRef name = py_getdict(module, py_name("__name__"));
    return name && py_isstr(name) ? std::string(view_of(name)) : std::string();
}

// --- The interpreter's callbacks ------------------------------------------------

void print_text(const char* text) {
    static std::string line; // what print wrote since its last line ended
    line += text;
    for (std::size_t end = line.find('\n'); end != std::string::npos; end = line.find('\n')) {
        KIN_LOG_INFO_F("script", "print", (LogFields{{.name = "text", .value = line.substr(0, end)}}));
        line.erase(0, end + 1);
    }
}

void mark_roots(void (*mark)(py_Ref value, void* context), void* context) {
    Host* host = current();
    if (!host) {
        return;
    }
    for (auto& [entity, value] : host->entities) {
        if (entity != 0) {
            mark(&value, context);
        }
    }
    for (auto& [id, info] : host->components) {
        for (auto& [entity, value] : info->views) {
            if (entity != 0) {
                mark(&value, context);
            }
        }
    }
    for (System& system : host->systems) {
        mark(&system.fn, context);
    }
    for (Handler& handler : host->handlers) {
        mark(&handler.fn, context);
    }
    for (QueuedEvent& event : host->events) {
        mark(&event.data, context);
    }
    for (QueuedEvent& event : host->delivering) {
        mark(&event.data, context);
    }
    for (auto& [id, info] : host->components) {
        mark(&info->start, context);
        mark(&info->stop, context);
    }
    for (py_TValue* kept : {&host->spawn, &host->due, &host->step, &host->fail, &host->wake, &host->cancel_module,
                            &host->running, &host->tween}) {
        mark(kept, context);
    }
}

char* import_no_file(const char*, int*) {
    return nullptr; // modules come from the folder only (lazy_import), and never as bytecode
}

py_GlobalRef lazy_import(const char* path);

// What scripts may import of what PocketPy builds in (the rest is taken out).
constexpr const char* kept_modules[] = {"math",      "vmath",   "easing", "json",       "enum",
                                        "traceback", "array2d", "base64", "unicodedata"};

} // namespace

Host* current() {
    const int vm = py_currentvm();
    return vm >= 0 && vm < static_cast<int>(runtime().hosts.size()) ? runtime().hosts[static_cast<std::size_t>(vm)] : nullptr;
}

bool no_host() {
    return py_exception(tp_RuntimeError, "kin is not running scripts now");
}

std::string module_of_function(py_Ref function) {
    return name_of_module(static_cast<py_GlobalRef>(kin_pocketpy_function_module(function)));
}

std::string module_running() {
    return name_of_module(py_inspect_currentmodule());
}

// --- Loading -------------------------------------------------------------------

namespace {

struct Loader {
    std::set<std::string> round;  // the modules this load (re)reads
    std::set<std::string> loaded; // those of them read already
    std::vector<std::string> ran; // those that read and ran
};

Loader*& active_loader() {
    static Loader* loader = nullptr;
    return loader;
}

} // namespace

void Host::problem(std::string_view module, std::string message, std::string traceback) {
    std::string& kept = problems[std::string(module)];
    if (kept == message) {
        return; // told already
    }
    kept = message;
    KIN_LOG_WARN_F("script", "python script error",
                   (LogFields{{.name = "module", .value = std::string(module)},
                              {.name = "error", .value = message},
                              {.name = "traceback", .value = std::move(traceback)}}));
}

namespace {

bool memory_left(Host& host, std::string_view module) {
    if (host.config.memory_limit == 0 || kin_pocketpy_memory_used() <= host.config.memory_limit) {
        return true;
    }
    py_gc_collect();
    if (kin_pocketpy_memory_used() <= host.config.memory_limit) {
        return true;
    }
    host.problem(module, "memory limit reached (" + std::to_string(kin_pocketpy_memory_used()) + " bytes held)");
    return false;
}

// Whatever `module` registered that is not of its generation `kept` goes.
void drop_registrations(Host& host, const std::string& module, u64 kept) {
    std::erase_if(host.systems, [&](const System& system) { return system.module == module && system.generation != kept; });
    std::erase_if(host.handlers, [&](const Handler& handler) { return handler.module == module && handler.generation != kept; });
    for (auto& [id, info] : host.components) {
        if (info->scripted && info->module == module && info->generation != kept) {
            // Defined by a read of it that is gone: what its class gave it goes too.
            const py_GlobalRef type_object = py_tpobject(info->type);
            for (const py_Name copied : info->copied) {
                py_deldict(type_object, copied);
            }
            info->copied.clear();
            py_newnil(&info->start);
            py_newnil(&info->stop);
            host.watch_start_stop(*info);
        }
    }
}

// Reads the module `name` of the folder and runs it in a new module object,
// which replaces its last version once it ran; when it fails, the last
// version keeps running (and what this read registered goes). False when it
// failed (the problem told).
bool execute(Host& host, Loader& loader, const std::string& name) {
    const std::string file = file_of_module(name);
    const std::filesystem::path full = host.config.content / host.config.folder / file;
    Module& record = host.modules[name];
    record.name = name;
    record.file = file;
    record.time = time_of(full);
    std::string source;
    if (!read_text(full, source)) {
        host.problem(name, file + ": the file could not be read");
        return false;
    }
    if (!memory_left(host, name)) {
        return false;
    }
    const u64 last_generation = record.generation;
    const u64 generation = ++host.generations;
    record.generation = generation;
    std::vector<std::string> last_imports = std::move(record.imports);
    record.imports.clear();
    const std::string internal = "kin script " + std::to_string(generation) + " " + name;
    const py_GlobalRef module = py_newmodule(internal.c_str());
    py_newstrv(py_emplacedict(module, py_name("__name__")), view_of(name));
    py_newstrv(py_emplacedict(module, py_name("__file__")), view_of(file));
    host.executing.push_back(name);
    const py_StackRef unwind = py_peek(0);
    bool ran = false;
    {
        const Budget budget{host};
        ran = py_exec(source.c_str(), file.c_str(), EXEC_MODE, module);
    }
    host.executing.pop_back();
    Module& done = host.modules[name]; // (the map may have grown while it ran)
    if (!ran) {
        auto [message, traceback] = describe_exception(kin_pocketpy_budget_stop());
        host.problem(name, std::move(message), std::move(traceback));
        py_clearexc(unwind);
        drop_registrations(host, name, last_generation);
        done.generation = last_generation;
        done.imports = std::move(last_imports);
        kin_pocketpy_forget_module(internal.c_str());
        return false;
    }
    drop_registrations(host, name, generation);
    if (!done.internal.empty()) {
        kin_pocketpy_forget_module(done.internal.c_str());
    }
    done.internal = internal;
    done.object = py_getmodule(internal.c_str());
    host.problems.erase(name);
    loader.ran.push_back(name);
    return true;
}

// A module of the folder imported (`import enemies`): the one loaded, or read now.
py_GlobalRef lazy_import(const char* path) {
    Host* host = current();
    const std::string name(path);
    if (!host || module_name_of(file_of_module(name)) != name) {
        return nullptr;
    }
    if (!host->executing.empty()) {
        std::vector<std::string>& imports = host->modules[host->executing.back()].imports;
        if (std::ranges::find(imports, name) == imports.end()) {
            imports.push_back(name);
        }
    }
    Loader* loader = active_loader();
    const bool due = loader && loader->round.contains(name) && !loader->loaded.contains(name);
    if (const auto found = host->modules.find(name); found != host->modules.end() && found->second.object && !due) {
        return found->second.object;
    }
    if (!loader || !std::filesystem::is_regular_file(host->config.content / host->config.folder / file_of_module(name))) {
        return nullptr;
    }
    loader->loaded.insert(name);
    if (!execute(*host, *loader, name)) {
        return nullptr; // its problem is told; the import fails
    }
    return host->modules[name].object;
}

} // namespace

// --- The host -------------------------------------------------------------------

namespace {

void set_up_interpreter(Host& host) {
    py_Callbacks* callbacks = py_callbacks();
    callbacks->importfile = import_no_file;
    callbacks->lazyimport = lazy_import;
    callbacks->print = print_text;
    callbacks->flush = [] {};
    callbacks->getchr = [] { return -1; };
    callbacks->gc_mark = mark_roots;
    callbacks->displayhook = nullptr;

    kin_pocketpy_keep_modules(kept_modules, static_cast<int>(std::size(kept_modules)));
    const py_GlobalRef builtins = py_getmodule("builtins");
    for (const char* name : {"exit", "input", "exec", "eval", "compile", "open", "breakpoint"}) {
        py_deldict(builtins, py_name(name));
    }
    // It reaches every type, those of the modules taken out too (stdc's reads memory).
    py_deldict(py_tpobject(tp_type), py_name("__subclasses__"));

    host.kin = py_newmodule("kin");
    host.time = py_newmodule("kin.time");
    host.input_module = py_newmodule("kin.input");
    host.world_module = py_newmodule("kin.world");
    bind_kin(host);

    const std::span<const unsigned char> prelude = kin_python_embedded("python_kin.py");
    const std::string source(reinterpret_cast<const char*>(prelude.data()), prelude.size());
    const py_StackRef unwind = py_peek(0);
    bool ran = false;
    {
        const Budget budget{host};
        ran = py_exec(source.c_str(), "kin.py", EXEC_MODE, host.kin);
    }
    if (!ran) {
        const std::string why = describe_exception(KIN_POCKETPY_RUNNING).second;
        py_clearexc(unwind);
        throw std::runtime_error("kin's Python prelude failed: " + why);
    }
    const auto keep = [&](py_TValue& out, const char* name) {
        const py_ItemRef found = py_getdict(host.kin, py_name(name));
        if (!found) {
            throw std::runtime_error(std::string("kin's Python prelude has no ") + name);
        }
        py_assign(&out, found);
    };
    keep(host.spawn, "_spawn");
    keep(host.due, "_due");
    keep(host.step, "_step");
    keep(host.fail, "_fail");
    keep(host.wake, "_wake");
    keep(host.cancel_module, "_cancel_module");
    keep(host.running, "_running");
    keep(host.tween, "_Tween");
    host.expose_components();
}

// The folder's modules, in name order.
std::vector<std::string> folder_modules(const Host& host) {
    std::vector<std::string> names;
    const std::filesystem::path root = host.config.content / host.config.folder;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".py") {
            const std::string name = module_name_of(std::filesystem::relative(it->path(), root, error));
            if (!name.empty()) {
                names.push_back(name);
            }
        }
    }
    std::ranges::sort(names);
    return names;
}

void call_on_reload(Host& host, const Module& module) {
    if (!module.object) {
        return;
    }
    if (const py_ItemRef hook = py_getdict(module.object, py_name("on_reload")); hook && py_callable(hook)) {
        py_TValue fn = *hook;
        host.call(&fn, 0, nullptr, module.name, "on_reload");
    }
}

} // namespace

Host::Host(EcsWorld& given, PythonScriptsConfig given_config) : world(given), config(std::move(given_config)) {
    Runtime& made = runtime();
    if (!made.initialized) {
        py_initialize();
        made.initialized = true;
    }
    for (int i = 1; i < static_cast<int>(made.hosts.size()); ++i) {
        if (!made.hosts[static_cast<std::size_t>(i)]) {
            vm = i;
            break;
        }
    }
    if (vm == 0) {
        throw std::runtime_error("PocketPy has no interpreter left: 15 PythonScripts are running");
    }
    made.hosts[static_cast<std::size_t>(vm)] = this;
    data_payload = world.raw().id<detail::DataComponentPayload>();
    const VmScope scope{vm};
    py_resetvm();
    try {
        set_up_interpreter(*this);
    } catch (...) {
        py_resetvm();
        made.hosts[static_cast<std::size_t>(vm)] = nullptr;
        throw;
    }
}

Host::~Host() {
    const VmScope scope{vm};
    closing = true;
    for (auto& [id, info] : components) {
        if (info->added) {
            info->added.destruct();
        }
        if (info->removed) {
            info->removed.destruct();
        }
        for (flecs::entity* observer : {&info->added_event, &info->removed_event, &info->destroy_watch}) {
            if (*observer) {
                observer->destruct();
            }
        }
    }
    systems.clear();
    handlers.clear();
    events.clear();
    entities.clear();
    components.clear();
    py_resetvm();
    runtime().hosts[static_cast<std::size_t>(vm)] = nullptr;
}

bool Host::call(py_Ref fn, int argc, py_Ref argv, std::string_view module, std::string_view what) {
    if (!memory_left(*this, module)) {
        return false;
    }
    const py_StackRef unwind = py_peek(0);
    bool ran = false;
    {
        const Budget budget{*this};
        ran = py_call(fn, argc, argv);
    }
    if (!ran) {
        auto [message, traceback] = describe_exception(kin_pocketpy_budget_stop());
        if (!what.empty() && message.find(what) == std::string::npos) {
            message = std::string(what) + ": " + message;
        }
        problem(module, std::move(message), std::move(traceback));
        py_clearexc(unwind);
    }
    return ran;
}

void Host::queue(std::string name, flecs::entity_t entity, py_Ref data) {
    QueuedEvent& made = events.emplace_back();
    made.name = std::move(name);
    made.entity = entity;
    if (data) {
        py_assign(&made.data, data);
    } else {
        py_newnone(&made.data);
    }
}

void Host::watch_start_stop(ComponentInfo& info) {
    const bool wanted = !py_isnil(&info.start) || !py_isnil(&info.stop);
    if (!wanted) {
        if (info.added) {
            info.added.destruct();
            info.removed.destruct();
            info.added = {};
            info.removed = {};
        }
        return;
    }
    if (!info.added) {
        ComponentInfo* watched = &info;
        info.added = world.raw()
                         .observer()
                         .with(static_cast<flecs::id_t>(info.id))
                         .event(flecs::OnAdd)
                         .each([watched](flecs::entity entity) { watched->to_start.push_back(entity.id()); });
        info.removed = world.raw()
                           .observer()
                           .with(static_cast<flecs::id_t>(info.id))
                           .event(flecs::OnRemove)
                           .each([this, watched](flecs::entity entity) {
                               // While the component is still there: stop(self) reads it.
                               if (closing || !watched->started.erase(entity.id()) || py_isnil(&watched->stop)) {
                                   return;
                               }
                               const VmScope scope{vm};
                               py_TValue args[1];
                               view_object(&args[0], entity.id(), *watched);
                               py_TValue fn = watched->stop;
                               call(&fn, 1, args, watched->module, watched->name + ".stop");
                           });
    }
    // Those that have it now and never started.
    world.raw().query_builder<>().with(static_cast<flecs::id_t>(info.id)).build().each([&](flecs::entity entity) {
        if (!info.started.contains(entity.id())) {
            info.to_start.push_back(entity.id());
        }
    });
}

std::string_view component_of_event(std::string_view event) {
    for (const std::string_view prefix : {std::string_view("added:"), std::string_view("removed:")}) {
        if (event.starts_with(prefix)) {
            return event.substr(prefix.size());
        }
    }
    return {};
}

void Host::watch_event(ComponentInfo& info, std::string_view event) {
    const flecs::id_t id = static_cast<flecs::id_t>(info.id);
    if (event.starts_with("added:") && !info.added_event) {
        info.added_event = world.raw().observer().with(id).event(flecs::OnAdd).each([this, name = std::string(event)](flecs::entity entity) {
            queue(name, entity.id(), nullptr);
        });
    } else if (event.starts_with("removed:") && !info.removed_event) {
        info.removed_event = world.raw().observer().with(id).event(flecs::OnRemove).each(
            [this, name = std::string(event)](flecs::entity entity) { queue(name, entity.id(), nullptr); });
    } else if (event == "destroyed" && !info.destroy_watch) {
        info.destroy_watch = world.raw().observer().with(id).event(flecs::OnRemove).each(
            [this, component = info.id](flecs::entity entity) { maybe_destroyed.emplace_back(entity.id(), component); });
    }
}

// What each entity's body began and stopped touching since the last update
// (PhysicsContactQueue), as events of the entity, the other entity their data.
void Host::collect_contacts() {
#if KIN_ENABLE_PHYSICS
    std::vector<flecs::entity> touched;
    world.raw().each([&](flecs::entity entity, const PhysicsContactQueue& queue) {
        if (!queue.events.empty()) {
            touched.push_back(entity);
        }
    });
    if (touched.empty()) {
        return;
    }
    std::vector<std::pair<PhysicsBody, flecs::entity_t>> bodies;
    world.raw().each([&](flecs::entity entity, const PhysicsBodyComponent& body) { bodies.emplace_back(body.body, entity.id()); });
    world.raw().each([&](flecs::entity entity, const TilemapCollider& map) { bodies.emplace_back(map.body, entity.id()); });
    const auto entity_of = [&](PhysicsBody body) -> flecs::entity_t {
        for (const auto& [candidate, entity] : bodies) {
            if (candidate == body) {
                return entity;
            }
        }
        return 0;
    };
    for (const flecs::entity entity : touched) {
        const PhysicsContactQueue* contacts = entity.get<PhysicsContactQueue>();
        const PhysicsBodyComponent* mine = entity.get<PhysicsBodyComponent>();
        for (const PhysicsContactEvent& event : contacts->events) {
            const PhysicsBody other = mine && event.body_a == mine->body ? event.body_b : event.body_a;
            py_TValue data{};
            if (const flecs::entity_t found = entity_of(other)) {
                entity_object(&data, found);
            } else {
                py_newnone(&data);
            }
            queue(event.type == PhysicsContactEventType::Begin ? "contact" : "contact_end", entity.id(), &data);
        }
    }
#endif
}

// --- Each update -------------------------------------------------------------------

namespace {

void run_starts(Host& host) {
    flecs::world_t* w = host.world.raw().c_ptr();
    for (auto& [id, owned] : host.components) {
        ComponentInfo& info = *owned;
        if (info.to_start.empty()) {
            continue;
        }
        std::vector<flecs::entity_t> pending;
        pending.swap(info.to_start);
        std::ranges::sort(pending); // the same order on every run
        for (const flecs::entity_t entity : pending) {
            if (!ecs_is_alive(w, entity) || !ecs_has_id(w, entity, info.id) || !info.started.insert(entity).second) {
                continue;
            }
            if (py_isnil(&info.start)) {
                continue;
            }
            py_TValue args[1];
            host.view_object(&args[0], entity, info);
            py_TValue fn = info.start;
            host.call(&fn, 1, args, info.module, info.name + ".start");
        }
    }
}

void dispatch(Host& host, const QueuedEvent& event) {
    const bool destroyed = event.via != 0;
    flecs::world_t* w = host.world.raw().c_ptr();
    py_TValue args[2];
    if (event.entity != 0) {
        host.entity_object(&args[0], event.entity);
    } else {
        py_newnone(&args[0]);
    }
    args[1] = event.data;
    const std::size_t count = host.handlers.size();
    for (std::size_t i = 0; i < count && i < host.handlers.size(); ++i) {
        const Handler& handler = host.handlers[i];
        if (handler.event != event.name) {
            continue;
        }
        if (destroyed) {
            if (handler.with.empty() || handler.with[0] != event.via) {
                continue; // (it is gone: what it had cannot be asked)
            }
        } else if (!handler.with.empty()) {
            if (event.entity == 0 || !ecs_is_alive(w, event.entity)) {
                continue;
            }
            const EcsEntity target{flecs::entity(w, event.entity)};
            if (!std::ranges::all_of(handler.with, [&](ComponentId id) { return host.world.components().has(target, id); })) {
                continue;
            }
        }
        py_TValue fn = handler.fn;
        const std::string module = handler.module;
        const std::string what = "@on(\"" + handler.event + "\") " + handler.name;
        host.call(&fn, 2, args, module, what);
    }
    py_TValue wake[3];
    py_newstrv(&wake[0], view_of(event.name));
    wake[1] = args[0];
    wake[2] = event.data;
    py_TValue fn = host.wake;
    host.call(&fn, 3, wake, "kin", "event " + event.name);
}

void deliver(Host& host) {
    if (!host.maybe_destroyed.empty()) {
        flecs::world_t* w = host.world.raw().c_ptr();
        std::vector<std::pair<flecs::entity_t, ComponentId>> watched;
        watched.swap(host.maybe_destroyed);
        for (const auto& [entity, component] : watched) {
            if (!ecs_is_alive(w, entity)) {
                QueuedEvent& made = host.events.emplace_back();
                made.name = "destroyed";
                made.entity = entity;
                made.via = component;
                py_newnone(&made.data);
            }
        }
    }
    for (i32 round = 0; round < std::max(host.config.event_rounds, 1) && !host.events.empty(); ++round) {
        host.delivering.swap(host.events);
        for (std::size_t i = 0; i < host.delivering.size(); ++i) {
            const QueuedEvent event = host.delivering[i];
            dispatch(host, event);
        }
        host.delivering.clear();
    }
    if (!host.events.empty()) {
        host.problem("events", "events still emitted after " + std::to_string(host.config.event_rounds) +
                                   " rounds of handlers (a loop?): the rest wait for the next update");
    }
}

void run_systems(Host& host) {
    flecs::world_t* w = host.world.raw().c_ptr();
    for (std::size_t i = 0; i < host.systems.size(); ++i) {
        run_starts(host); // an entity starts before its first update
        System& system = host.systems[i];
        std::vector<flecs::entity_t> matched;
        system.query.each([&](flecs::entity entity) { matched.push_back(entity.id()); });
        if (matched.empty()) {
            continue;
        }
        py_TValue fn = system.fn;
        const std::vector<ComponentInfo*> with = system.with;
        const std::vector<ComponentId> without = system.without;
        const bool method = system.method;
        const std::string module = system.module;
        const std::string name = system.name;
        if (!memory_left(host, module)) {
            continue;
        }
        const py_StackRef unwind = py_peek(0);
        std::optional<std::pair<std::string, std::string>> failed;
        {
            const Budget budget{host};
            for (const flecs::entity_t entity : matched) {
                // Visited as matched when the system began: one gone, or changed since, is left out.
                if (!ecs_is_alive(w, entity) ||
                    !std::ranges::all_of(with, [&](const ComponentInfo* info) { return ecs_has_id(w, entity, info->id); }) ||
                    std::ranges::any_of(without, [&](ComponentId id) { return ecs_has_id(w, entity, id); })) {
                    continue;
                }
                py_push(&fn);
                py_pushnil();
                if (method) {
                    host.view_object(py_pushtmp(), entity, *with[0]);
                    py_newfloat(py_pushtmp(), host.dt);
                } else {
                    py_newfloat(py_pushtmp(), host.dt);
                    for (ComponentInfo* info : with) {
                        host.view_object(py_pushtmp(), entity, *info);
                    }
                }
                const auto argc = static_cast<uint16_t>(method ? 2 : 1 + with.size());
                if (!py_vectorcall(argc, 0)) {
                    failed = describe_exception(kin_pocketpy_budget_stop());
                    break; // the rest of its entities wait for the next update
                }
            }
        }
        if (failed) {
            std::string message = failed->first;
            if (message.find(name) == std::string::npos) {
                message = name + ": " + message;
            }
            host.problem(module, std::move(message), std::move(failed->second));
            py_clearexc(unwind);
            if (i < host.systems.size()) {
                host.systems[i].failing = host.problems[module];
            }
        } else if (i < host.systems.size()) {
            host.systems[i].failing.clear();
        }
    }
}

void run_tasks(Host& host) {
    py_TValue fn = host.due;
    if (!host.call(&fn, 0, nullptr, "kin", "tasks")) {
        return;
    }
    if (!py_islist(py_retval())) {
        return;
    }
    const py_StackRef due = py_pushtmp(); // kept on the stack (marked) while each runs
    py_assign(due, py_retval());
    const int count = py_list_len(due);
    for (int i = 0; i < count; ++i) {
        py_TValue task = *py_list_getitem(due, i);
        std::string module = "kin";
        if (py_getattr(&task, py_name("_module")) && py_isstr(py_retval())) {
            module = std::string(view_of(py_retval()));
        } else {
            py_clearexc(nullptr);
        }
        const py_StackRef unwind = py_peek(0);
        bool ran = false;
        if (memory_left(host, module)) {
            const Budget budget{host};
            py_TValue step = host.step;
            ran = py_call(&step, 1, &task);
        }
        if (ran) {
            continue;
        }
        auto [message, traceback] = describe_exception(kin_pocketpy_budget_stop());
        py_clearexc(unwind);
        host.problem(module, message, std::move(traceback));
        py_TValue args[2];
        args[0] = task;
        py_newstrv(&args[1], view_of(message));
        py_TValue fail = host.fail;
        host.call(&fail, 2, args, "kin", "tasks");
    }
    py_pop();
}

} // namespace

// --- Loading and reloading --------------------------------------------------------

namespace {

// Reads `names` again (or for the first time), in order; a module imported by
// one before it is read when imported. Those that fail keep their last good
// version running.
void load(Host& host, const std::vector<std::string>& names, bool reloading) {
    Loader loader;
    loader.round.insert(names.begin(), names.end());
    if (reloading) {
        for (const std::string& name : names) {
            py_TValue fn = host.cancel_module; // its tasks run its last version's code
            py_TValue args[1];
            py_newstrv(&args[0], view_of(name));
            host.call(&fn, 1, args, "kin", "reload");
        }
    }
    Loader* const outer = active_loader();
    active_loader() = &loader;
    for (const std::string& name : names) {
        if (loader.loaded.insert(name).second) {
            execute(host, loader, name);
        }
    }
    active_loader() = outer;
    host.order.clear();
    for (const std::string& name : folder_modules(host)) {
        if (const auto found = host.modules.find(name); found != host.modules.end() && found->second.object) {
            host.order.push_back(name);
        }
    }
    if (reloading) {
        for (const std::string& name : loader.ran) {
            call_on_reload(host, host.modules[name]);
        }
    }
}

// The modules to read again for `changed`: them, and what imports them.
std::vector<std::string> with_importers(const Host& host, std::set<std::string> changed) {
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [name, module] : host.modules) {
            if (changed.contains(name)) {
                continue;
            }
            if (std::ranges::any_of(module.imports, [&](const std::string& used) { return changed.contains(used); })) {
                changed.insert(name);
                grew = true;
            }
        }
    }
    std::vector<std::string> names(changed.begin(), changed.end());
    return names;
}

void check_files(Host& host, bool everything) {
    std::set<std::string> changed;
    for (const std::string& name : folder_modules(host)) {
        const auto found = host.modules.find(name);
        if (everything || found == host.modules.end() ||
            time_of(host.config.content / host.config.folder / file_of_module(name)) != found->second.time) {
            changed.insert(name);
        }
    }
    if (!changed.empty()) {
        host.prefabs.clear(); // read again when next spawned
        load(host, with_importers(host, std::move(changed)), true);
    }
}

} // namespace

} // namespace kin::python_detail

namespace kin {

using namespace python_detail;

bool check_python_syntax(std::string_view code, std::string_view name, std::string& error) {
    Runtime& made = runtime();
    if (!made.initialized) {
        py_initialize();
        made.initialized = true;
    }
    const VmScope scope{0};
    const py_StackRef unwind = py_peek(0);
    const std::string source(code);
    const std::string file(name);
    if (py_compile(source.c_str(), file.c_str(), EXEC_MODE, false)) {
        error.clear();
        return true;
    }
    error = describe_exception(KIN_POCKETPY_RUNNING).first;
    py_clearexc(unwind);
    return false;
}

struct PythonScripts::Impl {
    Host host;

    Impl(EcsWorld& world, PythonScriptsConfig config) : host(world, std::move(config)) {}
};

PythonScripts::PythonScripts(EcsWorld& world, PythonScriptsConfig config)
    : _impl(std::make_unique<Impl>(world, std::move(config))) {
    Host& host = _impl->host;
    const VmScope scope{host.vm};
    load(host, folder_modules(host), false);
}

PythonScripts::~PythonScripts() = default;

void PythonScripts::update(Input& input, f32 dt) {
    Host& host = _impl->host;
    const VmScope scope{host.vm};
    struct Restore {
        Host& host;
        ~Restore() { host.input = nullptr; }
    } const restore{host};
    host.input = &input;
    host.dt = dt;
    host.now += dt;
    ++host.frame;
    py_newfloat(py_emplacedict(host.time, py_name("dt")), dt);
    py_newfloat(py_emplacedict(host.time, py_name("now")), host.now);
    py_newint(py_emplacedict(host.time, py_name("frame")), static_cast<py_i64>(host.frame));
    if (host.config.check_frames > 0 && ++host.frames_since_check >= host.config.check_frames) {
        host.frames_since_check = 0;
        check_files(host, false);
        host.expose_components();
    }
    if (host.config.update_tweens) {
        update_tweens(host.world, dt);
    }
    run_starts(host);
    host.collect_contacts();
    deliver(host);
    run_systems(host);
    deliver(host);
    run_tasks(host);
    deliver(host);
}

void PythonScripts::emit(std::string_view name) {
    _impl->host.queue(std::string(name), 0, nullptr);
}

void PythonScripts::emit(std::string_view name, EcsEntity entity) {
    _impl->host.queue(std::string(name), entity.raw().id(), nullptr);
}

void PythonScripts::reload() {
    Host& host = _impl->host;
    const VmScope scope{host.vm};
    check_files(host, true);
}

const std::map<std::string, std::string>& PythonScripts::problems() const {
    return _impl->host.problems;
}

std::vector<std::string> PythonScripts::modules() const {
    return _impl->host.order;
}

namespace {

std::string python_type_of(ComponentFieldKind kind) {
    switch (kind) {
    case ComponentFieldKind::Bool: return "bool";
    case ComponentFieldKind::I32:
    case ComponentFieldKind::I64:
    case ComponentFieldKind::U32:
    case ComponentFieldKind::U64: return "int";
    case ComponentFieldKind::F32:
    case ComponentFieldKind::F64: return "float";
    case ComponentFieldKind::String:
    case ComponentFieldKind::AssetRef: return "str";
    case ComponentFieldKind::Vec2f: return "vec2";
    case ComponentFieldKind::Vec2i: return "vec2i";
    case ComponentFieldKind::Color: return "color";
    case ComponentFieldKind::EntityRef: return "Optional[Entity]";
    case ComponentFieldKind::Points: return "list[vec2]";
    default: return "Any";
    }
}

bool is_identifier(std::string_view text) {
    return !text.empty() && !std::isdigit(static_cast<unsigned char>(text[0])) &&
           std::ranges::all_of(text, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

} // namespace

bool PythonScripts::write_stubs(const std::filesystem::path& file, std::string* error) const {
    const Host& host = _impl->host;
    const std::span<const unsigned char> base = kin_python_embedded("python_kin.pyi");
    std::string text(reinterpret_cast<const char*>(base.data()), base.size());
    std::vector<ComponentDescriptor> described = host.world.components().descriptors();
    std::ranges::sort(described, {}, &ComponentDescriptor::name);
    for (const ComponentDescriptor& descriptor : described) {
        const auto info = host.components.find(descriptor.id);
        if ((info != host.components.end() && info->second->scripted) || !is_identifier(descriptor.name)) {
            continue;
        }
        text += "\nclass " + descriptor.name + "(View):\n";
        bool any = false;
        for (const ComponentFieldDescriptor& field : descriptor.fields) {
            if (!is_identifier(field.name) || field.name == "entity") {
                continue;
            }
            text += "    " + field.name + ": " + python_type_of(field.kind) + "\n";
            any = true;
        }
        if (!any) {
            text += "    pass\n";
        }
    }
    std::error_code made;
    std::filesystem::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary);
    out << text;
    if (!out) {
        if (error) {
            *error = "could not write " + file.string();
        }
        return false;
    }
    return true;
}

std::size_t PythonScripts::tasks() const {
    Host& host = _impl->host;
    const VmScope scope{host.vm};
    py_TValue fn = host.running;
    if (!host.call(&fn, 0, nullptr, "kin", "tasks") || !py_isint(py_retval())) {
        return 0;
    }
    return static_cast<std::size_t>(py_toint(py_retval()));
}

} // namespace kin
