// Python scripting (PythonScripts, experimental; docs/python_scripting.md):
// views read and write components, kin's and those a script defines (in the
// registry, with their options); a component's start/update/stop and
// systems run in order over the entities they match; events reach their
// handlers (filtered by component), and a loop of them is cut; tasks wait on
// frames, time, conditions, events, tasks, sync and race, fail without
// stopping the others and are cancelled with their entity; a changed module
// is read again (its components' values kept, its importers too), a broken
// edit keeps the last good one; the sandbox has no files, eval or os, and a
// runaway loop or allocation stops its call; two worlds have their own.

#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/python.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// Written again, with a time no check has seen yet.
void rewrite(const fs::path& file, const std::string& text, int seconds) {
    write(file, text);
    fs::last_write_time(file, fs::file_time_type::clock::now() + std::chrono::seconds(seconds));
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::size_t count_of(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++count;
    }
    return count;
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

fs::path fresh_directory(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    return dir;
}

void print_problems(const kin::PythonScripts& scripts) {
    for (const auto& [module, problem] : scripts.problems()) {
        std::cerr << "  " << module << ": " << problem << '\n';
    }
}

void assert_clean(const kin::PythonScripts& scripts) {
    print_problems(scripts);
    assert(scripts.problems().empty());
}

std::string problem_of(const kin::PythonScripts& scripts, const std::string& module) {
    const auto found = scripts.problems().find(module);
    return found == scripts.problems().end() ? std::string() : found->second;
}

// The module every test's scripts share: a Record component on an entity
// "record", and note() to append to it (what the C++ side reads back).
const char* record_module = R"py(from kin import component, world

@component
class Record:
    text: str = ""

def note(text):
    record = world.single(Record)
    record.text = record.text + str(text) + ";"
)py";

kin::EcsEntity make_record(kin::EcsWorld& world) {
    const kin::EcsEntity made = world.entities().create({.name = "record"});
    assert(world.components().add(made, "Record"));
    return made;
}

std::string record_text(kin::EcsWorld& world) {
    const kin::EcsEntity found = world.entities().find_by_name("record");
    const std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(found, "Record");
    assert(snapshot && !snapshot->fields.empty());
    return std::get<std::string>(snapshot->fields[0].value);
}

void step(kin::PythonScripts& scripts, kin::Input& input, int updates, float dt = 0.1f) {
    for (int i = 0; i < updates; ++i) {
        scripts.update(input, dt);
    }
}

// Views of kin's components and of one a script defines; its fields in the
// registry, as scene files and the inspector see them.
void test_components() {
    const fs::path dir = fresh_directory("kin_python_components");
    write(dir / "scripts/game.py", R"py(from kin import component, field, world, Transform2D, vec2

@component
class Health:
    hp: int = 100
    most: int = field(100, min=1, max=999, label="Most", description="The most it heals to")
    title: str = "nobody"
    speed: float = 2.5
    home: vec2 = vec2(1, 2)
    awake: bool = True
    target: Entity = None

    def heal(self, amount):
        self.hp = min(self.hp + amount, self.most)

    @property
    def ratio(self):
        return self.hp / self.most

hero = world.create("hero")
t = hero.add(Transform2D, pos=vec2(3, 4))
h = hero.add(Health, hp=40, title="Hero")
assert h.hp == 40 and h.speed == 2.5 and h.title == "Hero" and h.awake
assert h.home.x == 1 and h.home.y == 2
h.heal(500)
assert h.hp == 100 and h.ratio == 1.0
assert hero[Health] is h and hero.get(Health) is h and hero.has(Health) and hero.has("Health")
assert hero.get("Transform2D") is t
t.pos += vec2(1, 0)
assert t.pos.x == 4 and t.pos.y == 4
assert t.entity == hero and h.entity is hero

def raises(kind, action):
    try:
        action()
    except kind:
        return True
    return False

def set_hp(value):
    h.hp = value

def set_nope():
    h.nope = 1

assert raises(TypeError, lambda: set_hp("lots"))
assert raises(AttributeError, set_nope)
assert raises(AttributeError, lambda: h.nope)
assert raises(TypeError, lambda: hero["NoSuchComponent"])
assert raises(KeyError, lambda: world.create("bare")[Health])
assert world.find("bare").get(Health) is None
assert raises(TypeError, lambda: Health())

h.target = hero
assert h.target == hero
other = world.create("other")
other.add(Health)
other[Health].hp -= 1
assert other[Health].hp == 99 and other[Health].title == "nobody"
other.remove(Health)
assert not other.has(Health)
kept = hero[Health]
hero.remove(Health)
assert raises(RuntimeError, lambda: kept.hp)
hero.add(Health, hp=7)
assert kept.hp == 7  # the same view, once it has one again
assert raises(TypeError, lambda: hero.add(Health, hp="seven"))
assert raises(AttributeError, lambda: hero.add(Health, nope=1))
assert repr(hero) == "<Entity hero #" + str(hero.id) + ">"
assert world.find("hero") == hero and world.find("nobody") is None
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir}};
    assert_clean(scripts);
    assert(scripts.modules() == std::vector<std::string>{"game"});

    const kin::ComponentDescriptor* health = world.components().find("Health");
    assert(health && health->kind == kin::ComponentKind::Data && health->fields.size() == 7);
    assert(health->fields[1].name == "most" && health->fields[1].label == "Most");
    assert(health->fields[1].min_value == 1.0 && health->fields[1].max_value == 999.0);
    assert(health->fields[4].kind == kin::ComponentFieldKind::Vec2f);
    assert(health->fields[6].kind == kin::ComponentFieldKind::EntityRef);

    std::string error;
    assert(scripts.write_stubs(dir / "scripts/kin.pyi", &error));
    std::ifstream stubs(dir / "scripts/kin.pyi", std::ios::binary);
    const std::string stub_text((std::istreambuf_iterator<char>(stubs)), std::istreambuf_iterator<char>());
    assert(contains(stub_text, "class Entity:") && contains(stub_text, "def sync(*items: Any) -> Awaitable: ..."));
    assert(contains(stub_text, "class Transform2D(View):\n    pos: vec2\n    rotation: float\n    scale: vec2\n"));
    assert(!contains(stub_text, "class Health(View)")); // the script's own: from its module

    const kin::EcsEntity hero = world.entities().find_by_name("hero");
    assert(near(hero.get<kin::Transform2D>()->pos.x, 4.0f) && near(hero.get<kin::Transform2D>()->pos.y, 4.0f));
    const std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(hero, "Health");
    assert(snapshot && std::get<kin::i32>(snapshot->fields[0].value) == 7);
}

// start/update/stop in order; systems with and without, over the entities
// matched when they begin; query.
void test_systems() {
    const fs::path dir = fresh_directory("kin_python_systems");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_game.py", R"py(from kin import component, system, query, Transform2D, vec2
from a_record import note

@component
class Frozen:
    pass

@component
class Mover:
    speed: float = 10

    def start(self):
        note("start " + self.entity.name)

    def update(self, dt):
        self.entity[Transform2D].pos += vec2(self.speed * dt, 0)

    def stop(self):
        note("stop " + self.entity.name + " " + str(int(self.speed)))

@system(Transform2D, Mover, without=Frozen)
def clamp(dt, t, m):
    if t.pos.x > 5:
        t.pos = vec2(5, t.pos.y)

@system(Transform2D)
def count(dt, t):
    if t.entity.name == "c":
        note("movers " + str(len(query(Mover))) + " frozen " + str(len(query(Mover, Frozen))))
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir}};
    assert_clean(scripts);
    make_record(world);
    kin::EcsEntity a = world.entities().create({.name = "a"});
    a.set(kin::Transform2D{});
    assert(world.components().add(a, "Mover"));
    kin::EcsEntity b = world.entities().create({.name = "b"});
    b.set(kin::Transform2D{});
    assert(world.components().add(b, "Mover"));
    assert(world.components().add(b, "Frozen"));
    world.entities().create({.name = "c"}).set(kin::Transform2D{});
    kin::Input input;
    step(scripts, input, 10);
    assert_clean(scripts);
    assert(near(a.get<kin::Transform2D>()->pos.x, 5.0f));  // moved, then clamped
    assert(near(b.get<kin::Transform2D>()->pos.x, 10.0f)); // frozen: not clamped
    world.components().remove(a, "Mover");
    world.entities().destroy(b);
    step(scripts, input, 1);
    assert_clean(scripts);
    const std::string text = record_text(world);
    assert(text.starts_with("start a;start b;movers 2 frozen 1;"));
    assert(count_of(text, "movers 2 frozen 1;") == 10);
    assert(contains(text, "stop a 10;stop b 10;"));
    assert(text.ends_with("movers 0 frozen 0;"));
}

// Handlers by name and component; events emitted by handlers delivered in
// the same update; a loop cut, told, and the game going on.
void test_events() {
    const fs::path dir = fresh_directory("kin_python_events");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_game.py", R"py(from kin import component, on, emit, world
from a_record import note

@component
class Health:
    hp: int = 3

@on("tick")
def tick(e, data):
    world.find("goblin").emit("hit", 2)
    world.find("rock").emit("hit", 2)

@on("hit", Health)
def hurt(e, damage):
    e[Health].hp -= damage
    note("hurt " + e.name + " " + str(e[Health].hp))
    if e[Health].hp <= 0:
        e.emit("died", e.name)

@on("hit")
def any_hit(e, damage):
    note("hit " + e.name)

@on("died")
def died(e, name):
    note("died " + name)
    emit("over")

@on("over")
def over(e, data):
    note("over " + str(e))

@on("loop")
def loop(e, data):
    emit("loop")
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir}};
    assert_clean(scripts);
    make_record(world);
    kin::EcsEntity goblin = world.entities().create({.name = "goblin"});
    assert(world.components().add(goblin, "Health"));
    world.entities().create({.name = "rock"});
    kin::Input input;
    scripts.emit("tick");
    step(scripts, input, 1);
    scripts.emit("tick");
    step(scripts, input, 1);
    assert_clean(scripts);
    assert(record_text(world) ==
           "hurt goblin 1;hit goblin;hit rock;hurt goblin -1;hit goblin;hit rock;died goblin;over None;");
    scripts.emit("loop");
    step(scripts, input, 1);
    assert(contains(problem_of(scripts, "events"), "a loop?"));
    scripts.emit("tick"); // still going
    step(scripts, input, 1);
    assert(contains(record_text(world), "hurt goblin -3;"));
}

// Tasks: each awaitable, results through yield from, a failing task told
// (and raised in the task waiting on it), cancelled with its entity.
void test_tasks() {
    const fs::path dir = fresh_directory("kin_python_tasks");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_game.py", R"py(from kin import on, emit, run, wait, frames, until, event, sync, race, world, TaskFailed
from a_record import note, Record

def child(count):
    yield frames(count)
    return count * 10

def failing():
    yield
    note("about to fail")
    return 1 / 0

def intro():
    note("a")
    yield
    note("b")
    yield wait(0.25)
    note("c")
    data = yield from event("go")
    note("go " + str(data))
    which, result = yield from race(wait(10), event("stop"))
    note("race " + str(which))
    results = yield from sync(child(1), child(2))
    note("sync " + str(results))
    yield until(lambda: world.single(Record).text.endswith("sync [10, 20];"))
    task = run(failing())
    try:
        yield from task
    except TaskFailed:
        note("failed " + str(task.error is not None))
    return 7

main = run(intro())

def watch():
    result = yield from main
    note("result " + str(result))

run(watch())

@on("signal")
def signal(e, data):
    emit("go", 5)

def forever(e):
    while True:
        note("tick " + e.name)
        yield

@on("start_forever")
def start_forever(e, data):
    spinner = world.find("spinner")
    spinner.run(forever(spinner))
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir}};
    make_record(world);
    kin::Input input;
    assert_clean(scripts);
    assert(scripts.tasks() == 2);
    step(scripts, input, 1);
    assert(record_text(world) == "a;");
    step(scripts, input, 1);
    assert(record_text(world) == "a;b;");
    step(scripts, input, 2);
    assert(record_text(world) == "a;b;");
    step(scripts, input, 1); // 0.25 s on
    assert(record_text(world) == "a;b;c;");
    step(scripts, input, 5);
    assert(record_text(world) == "a;b;c;");
    scripts.emit("signal");
    step(scripts, input, 1);
    assert(record_text(world) == "a;b;c;go 5;");
    step(scripts, input, 3);
    scripts.emit("stop");
    step(scripts, input, 20);
    const std::string text = record_text(world);
    assert(contains(text, "go 5;race 1;sync [10, 20];about to fail;failed True;result 7;"));
    assert(contains(problem_of(scripts, "b_game"), "ZeroDivisionError"));
    assert(scripts.tasks() == 0);

    kin::EcsEntity spinner = world.entities().create({.name = "spinner"});
    scripts.emit("start_forever");
    step(scripts, input, 3);
    assert(count_of(record_text(world), "tick spinner") == 3 && scripts.tasks() == 1);
    world.entities().destroy(spinner);
    step(scripts, input, 3);
    assert(count_of(record_text(world), "tick spinner") == 3 && scripts.tasks() == 0); // cancelled with it
}

// A module read again: its components keep their values (a new field gets
// its default), its systems are replaced, its tasks cancelled, on_reload
// called; a broken edit keeps the last good one; importers are read again.
void test_reload() {
    const fs::path dir = fresh_directory("kin_python_reload");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/helpers.py", "def factor():\n    return 1\n");
    write(dir / "scripts/game.py", R"py(from kin import component, system, run, wait
import helpers
from a_record import note

@component
class Counter:
    value: int = 0

@system(Counter)
def count(dt, c):
    c.value += helpers.factor()

def chatter():
    while True:
        yield wait(0.1)
        note("chat")

run(chatter())
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir, .check_frames = 1}};
    make_record(world);
    kin::EcsEntity counted = world.entities().create({.name = "counted"});
    assert(world.components().add(counted, "Counter"));
    kin::Input input;
    step(scripts, input, 3);
    assert_clean(scripts);
    const auto value = [&] {
        return std::get<kin::i32>(world.components().snapshot(counted, "Counter")->fields[0].value);
    };
    assert(value() == 3);

    // A new field, a system counting by 10, and on_reload: the value is kept.
    rewrite(dir / "scripts/game.py", R"py(from kin import component, system, world
import helpers
from a_record import note

@component
class Counter:
    value: int = 0
    label: str = "new"

@system(Counter)
def count(dt, c):
    c.value += 10 * helpers.factor()

def on_reload():
    note("reloaded " + world.find("counted")[Counter].label)
)py",
            10);
    const std::size_t chats = count_of(record_text(world), "chat");
    assert(chats > 0);
    step(scripts, input, 1);
    assert_clean(scripts);
    assert(value() == 13);
    assert(contains(record_text(world), "reloaded new;"));
    assert(world.components().find("Counter")->fields.size() == 2);
    step(scripts, input, 3);
    assert(value() == 43);
    assert(count_of(record_text(world), "chat") == chats); // its task went with its last version

    // Broken: the last good version keeps counting; told with its line.
    rewrite(dir / "scripts/game.py", "from kin import system\n\n@system(Counter\ndef count(dt, c):\n    pass\n", 20);
    step(scripts, input, 1);
    assert(contains(problem_of(scripts, "game"), "game.py:"));
    assert(value() == 53);
    rewrite(dir / "scripts/game.py", "import helpers\nraise ValueError('at load')\n", 30);
    step(scripts, input, 1);
    assert(contains(problem_of(scripts, "game"), "game.py:2: ValueError: at load"));
    assert(value() == 63);

    // A kind change is refused: told, and the component stays as it was.
    rewrite(dir / "scripts/game.py", R"py(from kin import component, system
import helpers

@component
class Counter:
    value: str = ""
    label: str = "new"

@system(Counter)
def count(dt, c):
    pass
)py",
            40);
    step(scripts, input, 1);
    assert(contains(problem_of(scripts, "game"), "Counter"));
    assert(value() == 73);

    // Fixed again; then a helper changed: its importer is read again too.
    rewrite(dir / "scripts/game.py", R"py(from kin import component, system
import helpers

@component
class Counter:
    value: int = 0
    label: str = "new"

@system(Counter)
def count(dt, c):
    c.value += helpers.factor()
)py",
            50);
    step(scripts, input, 1);
    assert_clean(scripts);
    assert(value() == 74);
    rewrite(dir / "scripts/helpers.py", "def factor():\n    return 100\n", 60);
    step(scripts, input, 1);
    assert_clean(scripts);
    assert(value() == 174);
    assert(scripts.modules() == (std::vector<std::string>{"a_record", "game", "helpers"}));
}

// No files, eval, os or sys; a runaway loop or allocation stops its call,
// not the game; a module that never ends its load is told.
void test_sandbox() {
    const fs::path dir = fresh_directory("kin_python_sandbox");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_checks.py", R"py(from kin import on
from a_record import note
import builtins
import math

@on("check")
def check(e, data):
    for name in ("open", "exec", "eval", "compile", "input", "exit", "breakpoint"):
        if hasattr(builtins, name):
            note("has " + name)
    for module in ("os", "sys", "io", "time", "random", "gc", "builtins_not_here", "a_record_nope"):
        try:
            __import__(module)
            note("imported " + module)
        except ImportError:
            pass
    note("math " + str(math.floor(2.5)))
)py");
    write(dir / "scripts/c_runaway.py", R"py(from kin import system, Transform2D
from a_record import note

@system(Transform2D)
def spin(dt, t):
    if t.entity.name == "spinner":
        while True:
            pass

@system(Transform2D)
def after(dt, t):
    note("after")
)py");
    write(dir / "scripts/e_grow.py", R"py(from kin import on

@on("grow")
def grow(e, data):
    held = []
    while True:
        held.append("x" * 4000)
)py");
    write(dir / "scripts/d_endless.py", "while True:\n    pass\n");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world,
                               {.content = dir, .instruction_limit = 1'000'000, .memory_limit = 16u * 1024u * 1024u}};
    make_record(world);
    assert(contains(problem_of(scripts, "d_endless"), "instruction limit reached"));
    print_problems(scripts);
    assert(problem_of(scripts, "b_checks").empty());
    world.entities().create({.name = "spinner"}).set(kin::Transform2D{});
    kin::Input input;
    scripts.emit("check");
    step(scripts, input, 2);
    assert(problem_of(scripts, "b_checks").empty());
    assert(contains(problem_of(scripts, "c_runaway"), "instruction limit reached"));
    scripts.emit("grow");
    step(scripts, input, 1);
    assert(contains(problem_of(scripts, "e_grow"), "memory limit reached"));
    step(scripts, input, 1);
    const std::string text = record_text(world);
    assert(contains(text, "math 2;"));
    assert(!contains(text, "has ") && !contains(text, "imported "));
    assert(count_of(text, "after") == 4); // the system after the runaway one ran every update
    std::string error;
    assert(kin::check_python_syntax("def f():\n    return 1\n", "ok.py", error) && error.empty());
    assert(!kin::check_python_syntax("def f(:\n", "bad.py", error) && contains(error, "bad.py:1"));
}

// Two worlds, each with its scripts: their own modules, components and state.
void test_two_worlds() {
    const fs::path dir = fresh_directory("kin_python_two_worlds");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_game.py", R"py(from kin import system
from a_record import Record
updates = 0

@system(Record)
def count(dt, r):
    global updates
    updates += 1
    r.text = str(updates)
)py");
    kin::EcsWorld first;
    kin::EcsWorld second;
    kin::register_builtin_editor_components(first);
    kin::register_builtin_editor_components(second);
    kin::PythonScripts one{first, {.content = dir}};
    kin::PythonScripts two{second, {.content = dir}};
    make_record(first);
    make_record(second);
    kin::Input input;
    step(one, input, 3);
    step(two, input, 1);
    step(one, input, 1);
    assert_clean(one);
    assert_clean(two);
    assert(record_text(first) == "4" && record_text(second) == "1");
}

// kin's events: a component added and removed, an entity destroyed (once
// gone); a tween waited on; with physics, what a body touches and leaves.
void test_engine_events() {
    const fs::path dir = fresh_directory("kin_python_engine_events");
    write(dir / "scripts/a_record.py", record_module);
    write(dir / "scripts/b_game.py", R"py(from kin import component, on, world, Transform2D, vec2
from a_record import note

@component
class Coin:
    value: int = 5

@on("added:Coin")
def added(e, data):
    note("added " + e.name + " " + str(e[Coin].value))

@on("removed:Coin")
def removed(e, data):
    note("removed alive " + str(e.alive))

@on("destroyed", Coin)
def gone(e, data):
    note("destroyed alive " + str(e.alive))

def fade(e):
    yield e.tween("Transform2D.scale", vec2(0, 0), 0.5, ease="linear")
    note("faded " + str(e[Transform2D].scale.x))
    e.destroy()

@on("fade")
def start_fade(e, data):
    coin = world.find("Coin")
    coin.run(fade(coin))

@on("contact")
def touched(e, other):
    note(e.name + " touched " + (other.name if other is not None else "?"))

@on("contact_end")
def left(e, other):
    note(e.name + " left " + (other.name if other is not None else "?"))
)py");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir}};
    assert_clean(scripts);
    make_record(world);
    kin::EcsEntity coin = world.entities().create({.name = "Coin"});
    coin.set(kin::Transform2D{});
    assert(world.components().add(coin, "Coin"));
    kin::Input input;
    step(scripts, input, 1);
    scripts.emit("fade");
    step(scripts, input, 4);
    assert(!contains(record_text(world), "faded"));
    step(scripts, input, 3);
    assert_clean(scripts);
    assert(record_text(world) == "added Coin 5;faded 0.0;removed alive False;destroyed alive False;");

#if KIN_ENABLE_PHYSICS
    kin::PhysicsWorld physics{{.gravity = {0.0f, 9.8f * 32.0f}, .units_per_meter = 32.0f}};
    kin::EcsPhysicsBridge bridge{world, physics};
    world.entity("Floor")
        .set(kin::Transform2D{.pos = {0.0f, 116.0f}})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({400.0f, 16.0f})}}}});
    world.entity("Gate")
        .set(kin::Transform2D{})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({16.0f, 16.0f}), .sensor = true}}}});
    world.entity("Hero")
        .set(kin::Transform2D{})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({8.0f, 16.0f})}}}})
        .set(kin::PhysicsContactQueue{});
    for (int frame = 0; frame < 90; ++frame) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
        scripts.update(input, 1.0f / 60.0f);
    }
    assert_clean(scripts);
    const std::string text = record_text(world);
    assert(count_of(text, "Hero touched Gate;") == 1 && count_of(text, "Hero left Gate;") == 1);
    assert(count_of(text, "Hero touched Floor;") == 1 && !contains(text, "Hero left Floor;"));
    assert(text.find("Hero left Gate;") < text.find("Hero touched Floor;"));
#endif
}

} // namespace

int main() {
    test_components();
    test_systems();
    test_events();
    test_tasks();
    test_reload();
    test_engine_events();
    test_sandbox();
    test_two_worlds();
    std::puts("python scripting tests passed");
    return 0;
}
