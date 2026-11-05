# Python scripting (experimental)

Gameplay in Python, from three ideas: **components** (what an entity has),
**tasks** (what happens over time) and **events** (what happens in reaction).
Nothing else becomes a concept until a game shows it is worth it.

- `kin::PythonScripts` (`kin/scripting/python.hpp`, target `kin::python_scripts`),
  built with the PYTHON component (`-DKIN_ENABLE_PYTHON=ON`, off by default).
- PocketPy 2.2.0, a Python 3 interpreter in C, runs it in a sandbox with an
  exact per-call budget.
- Lua scripting is separate and does not change.
- A sample game: `examples/python_garden` (below).

Python reaches every component through the descriptors that scene files and
the editor use, so a new C++ component is usable from Python with no binding
code. State lives in components, never only in a script, so reloading code
cannot lose it, and saves, the inspector and replay see it.

## Running scripts

```cpp
#include <kin/scripting/python.hpp>

kin::PythonScripts scripts{world, {.content = content_root}}; // loads content/scripts/*.py
scripts.emit("start");                                       // an event for the scripts
// each frame, after physics:
scripts.update(ctx.input, ctx.dt);
```

- **Loading.** Every module of the folder (`scripts/` by default) is loaded
  in name order. Modules import each other (`import
  enemies`) from that folder only. Make it before loading scenes that use the
  scripts' components, and destroy it before its world.
- **Each update, in this order:**
  1. kin's tweens (`update_tweens`; `update_tweens = false` when the game
     steps them itself);
  2. the `start` of components entities gained;
  3. the events queued since the last update (with what bodies touched);
  4. systems and `update` methods;
  5. the events they emitted;
  6. tasks whose wait is over;
  7. the events the tasks emitted.
- **Problems.** What a script raises is logged with its traceback and kept in
  `problems()` (module → `file.py:line: what`), and that call is skipped;
  the game goes on.
- **Interpreters.** Each `PythonScripts` has its own; PocketPy has 15 to
  give. Everything runs on the thread that made it.

## Components

### kin's

Each described component is a name in `kin` (`from kin import Transform2D,
RectRenderer`); its name as a string works too (`e["Transform2D"]`).

```python
t = e[Transform2D]          # a view: reads and writes go to the entity; KeyError without it
t.pos += vec2(4, 0)         # vec2 is a value: this reads, adds, writes
e.get(Transform2D)          # the view, or None
e.has(Health)
e.add(Health, hp=50)        # the view; the other fields keep their defaults
e.remove(Health)
```

- **What a view is.** It holds the entity and the component, nothing
  copied. Each entity's view of a component is one object (`is` holds).
  - Using it once the entity is gone, or the component removed, raises
    `RuntimeError`.
  - A field nobody described raises `AttributeError`; a value of the wrong
    kind, `TypeError`.
  - A view has no other attributes: a component keeps what it holds in its
    fields.
- **Writes.** A write goes into the component in place and is announced to
  kin as a change (as Lua's writes and C++'s `set` are). Writing a body's
  `Transform2D` moves the body too.

### A script's own

```python
@component
class Health:
    hp: int = 100
    most: int = field(100, min=1, label="Most")

    def heal(self, amount):
        self.hp = min(self.hp + amount, self.most)
```

- **What it becomes.** The class becomes a kin data component, through kin's
  component schema (`kin.components/1`). It is in the inspector, scene files
  and saves; C++ and Lua can query it.
- **Its type.** The decorator returns the type of its views: `Health(...)`
  cannot be called, and methods, properties and constants of the class are
  the views'.
- **Field options.** `field(default, min=, max=, label=, description=,
  units=, hidden=, readonly=, choices=, asset_type=)`.
- **Reload.** Reloading a module that changed a component migrates it: new
  fields get their defaults, removed ones go, the rest keep their values. A
  field whose kind changed is refused (told as a problem), and the last
  version keeps running.

| Annotation | Field kind |
|---|---|
| `bool` | Bool |
| `int`, `i64` | I32, I64 |
| `float`, `f64` | F32, F64 |
| `str` | String |
| `vec2`, `vec2i` | Vec2f, Vec2i |
| `color` | Color |
| `Entity` | EntityRef (`None` for none) |
| `asset` | AssetRef (`field("", asset_type="Image")` for its kind) |
| `list[vec2]` | Points |

A field cannot be named `entity`: a view's entity is `view.entity`.

### Methods: behaviour on a component

For one-off things such as the player: the "script on an object" of Unity,
still just a component.
- `update(self, dt)` runs each update for every entity that has the
  component.
- `start(self)` runs before an entity's first `update`.
- `stop(self)` runs when it loses the component or is destroyed, while the
  fields can still be read.

```python
@component
class Beetle:
    centre: vec2 = vec2(0, 0)
    walk: float = 100.0
    speed: float = 70.0

    def update(self, dt):
        t = self.entity[Transform2D]
        x = t.pos.x + self.speed * dt
        if abs(x - self.centre.x) > self.walk:
            self.speed = -self.speed
        t.pos = vec2(x, t.pos.y)
```

### Systems: one function over many entities

```python
@system(CharacterController2D, Gardener, without=Frozen)
def move(dt, body, g):
    body.velocity = vec2(input.axis("move") * 220.0, body.velocity.y)
```

- **Arguments.** `dt`, then a view of each component listed, in order.
- **Order.** Systems and `update` methods run in the order their modules
  loaded and defined them.
- **What a run visits.** The entities that matched when it began. One
  destroyed before its turn, or one that lost or gained a component so it
  no longer matches, is skipped. Changes made by the system (spawning,
  destroying, adding, removing) take effect at once; entities that come to
  match are visited next update.
- **Without a system.** `query(A, B, without=C)` is the same search: views
  for one component, tuples of views for several.

## Tasks

A task is a generator. Each `yield` waits on something, and the task resumes
once that is done.

```python
def fade(e):
    yield e.tween("Transform2D.scale", vec2(2.5, 2.5), 0.25, ease="ease_out")
    yield e.tween("RectRenderer.color", color(250, 240, 90, 0), 0.2)
    e.destroy()

e.run(fade(e))          # belongs to e
run(intro())            # belongs to its module
```

| `yield` on | Done when | Result (`r = yield from it`) |
|---|---|---|
| nothing (bare `yield`) | the next update | |
| `wait(seconds)` | that much game time passed | |
| `frames(n)` | n updates passed | |
| `until(fn)` | `fn()` is true (asked once an update) | |
| `event(name, entity=None)` | the event is emitted (by `entity`) | its data |
| `e.tween(...)` | the tween ended | |
| a `Task`, or a generator (made a task) | it ended | its return value |
| `sync(a, b, ...)` | all are done | their results, a list |
| `race(a, b, ...)` | the first is done; the others are cancelled | `(index, result)` |

- **Results.** `r = yield from x` waits as `yield x` does and gives the
  result. PocketPy has no `x = yield`.
- **Failures.** A task that raised raises `TaskFailed` in the task doing
  `yield from` on it; one cancelled raises `Cancelled`. A plain `yield` on it
  only waits.
- **Tweens.** `e.tween(field, to, seconds, ease="ease_out", delay=0.0,
  repeats=0, yoyo=False, relative=False)` tweens one field of a component.
  - kin's tweens do the work; the task only waits. `.stop()` stops one.
  - Easings: `linear`, `ease_in`, `ease_out`, `ease_in_out`, `sine_*`,
    `cubic_*`, `back_*`, `elastic_*`, `bounce_*`, `step` (kin's names, with or
    without the underscores).
- **Ownership.**
  - `e.run(...)` belongs to `e`: destroying it cancels the task.
  - `run(...)` belongs to its module: reloading the module cancels it.
  - A task that a task waits on (through `sync`, `race` or a generator)
    belongs to that task.
  - Cancelling a task cancels what it waits on (a tween stops where it is)
    and the tasks it started.
- **Cleanup.** PocketPy has no `finally`, so ownership is the cleanup.
- **The rest of a task's API.** `task.cancel()`, `task.done`, `task.result`,
  `task.error`, `task.cancelled`.
- **Budget.** Each resumption, to the next `yield`, runs under the call
  budget: a task looping without yielding is stopped and told.

**Tasks are not saved.** A paused generator is a Python stack: it cannot be
saved, copied or rolled back.
- Anything that must survive a save or a reload goes in a component.
- A task should be one that can start again from components.
- Gameplay that must replay exactly (rollback) goes in systems; tasks drive
  sequences and presentation.

## Events

```python
@on("seed_picked")                     # every one
def picked(e, data): ...

@on("contact", Gardener)               # only those of entities with Gardener
def touched(e, other):
    if other is not None and other.has(Seed):
        seeds.pick(other)

emit("level_done", {"time": 63.2})     # the world's: handlers get (None, data)
e.emit("gardener_lost")                # e's: handlers get (e, data)
# and from C++: scripts.emit("start"), scripts.emit("hit", entity)
```

- **Names and data.** Names are strings; data is any value (`None` by
  default).
- **Filtering.** `@on(name, A, B)` hears the events of entities that have `A`
  and `B`; `@on(name)` hears them all.
- **Delivery.** Events are queued and delivered in the order emitted, at the
  points listed under "Each update". One emitted by a handler is delivered in
  the same update, for up to `event_rounds` rounds; after that, what is left
  waits for the next update and the loop is told.
- **From tasks.** `yield event(name, entity)` waits for the same events.

| kin's events | Of | Data |
|---|---|---|
| `contact` / `contact_end` | an entity whose body has a `PhysicsContactQueue`, as it begins or stops touching another (sensors too) | the other entity (`None`: none has the body) |
| `added:Name` / `removed:Name` | an entity, when it gains or loses component `Name` | |
| `destroyed` (with a component: `@on("destroyed", Coin)`) | an entity that had it, once it is gone (`e.alive` is False) | |

To read an entity's fields as it goes, use its component's `stop`.

## The rest of the API

Each of these is a real operation, not component access:

- `world`:
  - `create(name="")`, an empty entity;
  - `spawn(prefab, at=None)`, a prefab's root;
  - `find(name)`;
  - `single(Component)`, the view of the one entity with it (the place for a
    game's own state);
  - `query(...)`.
- `input`: `held`, `pressed`, `released` (actions), `axis(name)`, `mouse()`.
- `time`: `dt`, `now`, `frame`.
- Entities:
  - `id`, `name`, `alive`, `parent`, `children`, `child(name)`;
  - `destroy()`, `emit`, `run`, `tween`, and the component calls above.
- `print`, to kin's log.

The next ones come when a game needs them: physics queries, scene changes,
saves, UI. Each would be a small module like `input`.

## Reload

Each `check_frames` updates (30), a module whose file changed is read again,
with the modules that import it:
- its tasks are cancelled;
- its systems, handlers and methods are replaced;
- its components migrate;
- its module-level variables start again;
- then its `on_reload()` runs, if it has one.

A module that fails to read (a syntax error, an exception at load, a refused
component change) keeps its last good version running, and its problem is
told. `reload()` reads all of them again.

## The sandbox

- **Builtins.** Python's, without `open`, `exec`, `eval`, `compile`, `input`,
  `exit`, `breakpoint` and `type.__subclasses__`.
- **Modules.** Of those PocketPy builds in, only `math`, `vmath`, `easing`,
  `json`, `enum`, `traceback`, `array2d`, `base64`, `unicodedata` and its
  pure-Python ones. There is no `os`, `sys`, `io`, `time`, `random` or `gc`,
  no file but the folder's modules, and no compiled bytecode or native
  libraries.
- **Instruction budget.** Each call (a system's run, a handler, a task's step,
  a module's load) may run `instruction_limit` bytecode instructions,
  counted the same on every run and machine.
- **Memory.** The interpreter may hold `memory_limit` bytes. A call that goes
  past either limit stops at its next instruction and is told.
- **One allocation is not stopped.** A single native operation runs to its
  end before the memory limit can stop it: `[0] * 10**9` takes the memory
  first. PocketPy does not survive a failed allocation, so kin cannot refuse
  one.

## Editors

`scripts.write_stubs("content/scripts/kin.pyi")` writes the types of `kin`,
with a class for each component the world describes, so VS Code (Pylance or
pyright) completes and checks the scripts. The scripts' own components come
from their modules. The sample game writes it when it runs with a window.
`check_python_syntax(code, name, error)` tells whether code reads, with
`name:line` where it does not.

## PocketPy's Python

PocketPy 2.2.0 is a subset of Python. What scripts must do without:

| Missing | Instead |
|---|---|
| `x = yield v`, a generator's `send`/`throw`/`close` | `r = yield from awaitable` |
| `finally`, `try`/`else` | ownership cleans up tasks; plain `try`/`except` |
| `nonlocal`, keyword-only arguments (`def f(a, *, b)`), `async`/`await` | module-level `global`; `def f(*a, b=1)` works |
| a generator expression as an argument (`all(x for x in y)`) | a list: `all([x for x in y])` |
| `a if p else b if q else c` without parentheses | `if`/`elif` |
| `dict.setdefault`, `{:,}` in formats | `get` and assign; plain `{}` |
| `-v` on a `vec2` | `v * -1` |

There: generators and `yield from` with a return value, decorators, class
annotations, `dataclasses`, `property`, `match`, `with`, the walrus, f-strings,
and `vec2`, a value (made without allocating, read-only: `with_x(3)` gives a
new one).

## The sample: Python garden

`examples/python_garden`: a platformer whose gameplay is all Python.
- **The game.** The gardener picks the seeds on the beds while beetles walk
  below; three lives, and R plays again.
- **The C++ host** (`main.cpp`) makes the world, physics and scripts, steps
  them and draws the world.
- **The scripts:**
  - `garden.py` builds the garden from kin's components;
  - `gardener.py` holds the character controller, keys and autoplay;
  - `seeds.py` and `beetles.py` hold the pickups and the enemies;
  - `game.py` holds the rules, lives and messages;
  - `hud.py` keeps two `TextRenderer`s up to date.

Run it with `python_garden`, edit a script while it runs, and the change is
read within half a second. CTest's `kin-python-garden` runs 900 headless
updates with the gardener playing itself (`autoplay`). It fails when a script
raises, when nothing is picked in 600 updates, or when R does not start the
garden again.

## Cost

`kin_python_bench` (Release, this machine) moves and bounces 10,000 entities,
each with `Transform2D` and a script's `Velocity`, every update:

| | ms per update |
|---|---|
| Python `@system` | 10.4 |
| Python `update(self)` | 10.4 |
| C++, the same components | 0.49 |

The PocketPy prototype measured Lua's entity scripts at 37.7 ms on a similar
test. The rest of the time is split:
- PocketPy's interpreter and attribute lookups take about half;
- kin's ECS change log takes about 15%: every component set records an event,
  for Lua's writes and C++'s `set` too, so it is an engine-wide cost;
- the rest is the views' field access.

A few hundred scripted entities cost well under a millisecond.

## Next

What the sample showed it could use, in the order a second game would likely
need them:
- physics queries (`raycast`, overlaps);
- scene changes and prefab-built levels (the garden is built in code);
- saves (components flagged to save).

Further off: Studio's view of scripts, a debugger, browser builds (PocketPy
compiles with Emscripten, untried here) and MSVC (built by CI only).
