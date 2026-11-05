# The part of the module `kin` written in Python: tasks and what they wait
# on, and the decorators that need nothing native beyond registering.
# python_host.cpp runs it in `kin` once kin's native functions are in it
# (python_api.cpp). A name starting with _ is kin's own. PocketPy runs it, so
# only its Python: no `finally`, no `x = yield`, no `nonlocal`.


class Cancelled(Exception):
    """Raised by `yield from task` when the task was cancelled."""


class TaskFailed(Exception):
    """Raised by `yield from task` when the task raised: its message."""


class field:
    """A component field with options: `hp: int = field(100, min=0)`."""

    def __init__(self, default=None, min=None, max=None, label="", description="", units="",
                 hidden=False, readonly=False, choices=None, asset_type=""):
        self.default = default
        self.min = min
        self.max = max
        self.label = label
        self.description = description
        self.units = units
        self.hidden = hidden
        self.readonly = readonly
        self.choices = choices
        self.asset_type = asset_type


def system(*components, without=None):
    """@system(A, B, without=C): the function runs each frame for every entity
    with A and B (and not C), given dt and a view of each."""
    if len(components) == 0 or not all([_is_component(c) for c in components]):
        raise TypeError("system() takes the components its entities have: @system(Transform2D, Patrol)")
    if without is None:
        without = ()
    elif type(without) is not tuple:
        without = (without,)

    def register(fn):
        _add_system(fn, components, without)
        return fn
    return register


def on(name, *components):
    """@on("hit", Health): the function runs for each "hit" event of an entity
    with Health, given the entity and the event's data."""
    if type(name) is not str:
        raise TypeError("on() takes the event's name first: @on(\"hit\")")

    def register(fn):
        _add_handler(fn, name, components)
        return fn
    return register


# --- What a task waits on ---------------------------------------------------
#
# Each has `done` and `result`. A task waiting on one calls `_start(task)`
# when it begins to wait, and the scheduler `_poll()` each frame until it is
# done; `_cancel()` when the task waiting is cancelled. `yield from` on one
# waits too and gives its result (or raises what its task raised).

class Awaitable:
    done = False
    result = None

    def __iter__(self):
        yield self
        return self._outcome()

    def _outcome(self):
        return self.result

    def _start(self, task):
        pass

    def _poll(self):
        return self.done

    def _cancel(self):
        pass


class _Frames(Awaitable):
    def __init__(self, count):
        self.count = count
        self.until = 0

    def _start(self, task):
        self.until = time.frame + self.count

    def _poll(self):
        if not self.done and time.frame >= self.until:
            self.done = True
        return self.done


class _Wait(Awaitable):
    def __init__(self, seconds):
        self.seconds = seconds
        self.until = 0.0

    def _start(self, task):
        self.until = time.now + self.seconds

    def _poll(self):
        # A step of game time is a float sum: a little under still counts.
        if not self.done and time.now >= self.until - 1e-6:
            self.done = True
        return self.done


_waiting_events = {}  # name: [the _Event waits for it]


class _Event(Awaitable):
    def __init__(self, name, entity):
        self.name = name
        self.entity = entity

    def _start(self, task):
        waits = _waiting_events.get(self.name)
        if waits is None:
            waits = []
            _waiting_events[self.name] = waits
        waits.append(self)

    def _cancel(self):
        waits = _waiting_events.get(self.name)
        if waits is not None and self in waits:
            waits.remove(self)


class _Tween(Awaitable):
    def __init__(self, entity, id):
        self.entity = entity
        self.id = id

    def _poll(self):
        if not self.done and not _tween_running(self.entity, self.id):
            self.done = True
        return self.done

    def _cancel(self):
        if not self.done:
            _tween_stop(self.entity, self.id)
            self.done = True

    def stop(self):
        """Stops the tween where it is."""
        self._cancel()


def wait(seconds):
    """Done once that much game time has passed."""
    return _Wait(float(seconds))


def frames(count):
    """Done once that many frames have passed."""
    return _Frames(int(count))


def until(condition):
    """Done once condition() is true (asked once a frame)."""
    while not condition():
        yield


def event(name, entity=None):
    """Done when the event is emitted (by `entity`, when given); its result is
    the event's data."""
    return _Event(name, entity)


class _Sync(Awaitable):
    def __init__(self, items):
        self.items = items
        self.parts = []

    def _start(self, task):
        self.parts = [_awaitable(item, task) for item in self.items]
        for part in self.parts:
            part._start(task)

    def _poll(self):
        if self.done:
            return True
        for part in self.parts:
            if not part._poll():
                return False
        self.done = True
        self.result = [part.result for part in self.parts]
        return True

    def _outcome(self):
        for part in self.parts:
            part._outcome()  # a part that failed raises
        return self.result

    def _cancel(self):
        for part in self.parts:
            part._cancel()


class _Race(Awaitable):
    def __init__(self, items):
        self.items = items
        self.parts = []
        self.winner = None

    def _start(self, task):
        self.parts = [_awaitable(item, task) for item in self.items]
        for part in self.parts:
            part._start(task)

    def _poll(self):
        if self.done:
            return True
        for index, part in enumerate(self.parts):
            if part._poll():
                self.done = True
                self.winner = part
                self.result = (index, part.result)
                for other in self.parts:
                    if other is not part:
                        other._cancel()
                return True
        return False

    def _outcome(self):
        if self.winner is not None:
            self.winner._outcome()
        return self.result

    def _cancel(self):
        for part in self.parts:
            part._cancel()


def sync(*items):
    """Done when every one is done; its result is theirs, a list."""
    return _Sync(list(items))


def race(*items):
    """Done when the first is done, and the others are cancelled; its result
    is (which, its result)."""
    if len(items) == 0:
        raise TypeError("race() takes what to wait on")
    return _Race(list(items))


# --- Tasks -------------------------------------------------------------------

def _g():
    yield


_generator = type(_g())
_tasks = []        # every task not yet done, in the order they began
_current = None    # the task running now


class Task(Awaitable):
    """A generator kin runs: each `yield` waits on something (wait(1),
    event("hit"), another task...), and the task goes on once it is done."""

    def __init__(self, generator, owner, module, parent):
        self._generator = _drive(generator, self)
        self._owner = owner      # the entity it belongs to, or None
        self._module = module    # the module that started it
        self._parent = parent    # the task it belongs to, or None
        self._waiting = None
        self._children = []
        self.cancelled = False
        self.error = None
        _tasks.append(self)
        if parent is not None:
            parent._children.append(self)

    def cancel(self):
        """Stops it, what it waits on and the tasks it started."""
        _cancel(self)

    def _cancel(self):
        _cancel(self)

    def _outcome(self):
        if self.cancelled:
            raise Cancelled("the task was cancelled")
        if self.error is not None:
            raise TaskFailed(self.error)
        return self.result

    def __repr__(self):
        state = "running"
        if self.cancelled:
            state = "cancelled"
        elif self.error is not None:
            state = "failed"
        elif self.done:
            state = "done"
        return "<Task " + state + ">"


def _drive(generator, task):
    task.result = yield from generator


def _awaitable(value, task):
    if value is None:
        return _Frames(1)
    if isinstance(value, Awaitable):
        return value
    if type(value) is _generator:
        return Task(value, task._owner, task._module, task)
    raise TypeError("a task waits on wait(), frames(), event(), a tween, a task, sync() or race(), not " +
                    type(value).__name__)


def _spawn(generator, owner, module):
    if type(generator) is not _generator:
        raise TypeError("run() takes a generator: run(intro()), not run(intro)")
    return Task(generator, owner, module, None)


def _cancel(task):
    if task.done:
        return
    task.done = True
    task.cancelled = True
    if task._waiting is not None:
        task._waiting._cancel()
        task._waiting = None
    for child in task._children:
        _cancel(child)
    task._generator = None


def _due():
    # The tasks to resume now, in the order they began; those whose entity is
    # gone are cancelled.
    global _tasks
    due = []
    kept = []
    for task in _tasks:
        if task.done:
            continue
        owner = task._owner
        if owner is not None and not owner.alive:
            _cancel(task)
            continue
        kept.append(task)
        waiting = task._waiting
        if waiting is None or waiting._poll():
            due.append(task)
    _tasks = kept
    return due


def _step(task):
    # Resumes it to its next yield. What it raises goes to kin, which tells it
    # and calls _fail.
    global _current
    if task.done:
        return
    _current = task
    task._waiting = None
    try:
        value = next(task._generator)
    except StopIteration:
        _current = None
        task.done = True
        task._generator = None
        return
    waiting = _awaitable(value, task)
    task._waiting = waiting
    waiting._start(task)
    _current = None


def _fail(task, message):
    global _current
    _current = None
    if task.done:
        return
    task.done = True
    task.error = message
    task._generator = None
    if task._waiting is not None:
        task._waiting._cancel()
        task._waiting = None
    for child in task._children:
        _cancel(child)


def _wake(name, entity, data):
    waits = _waiting_events.get(name)
    if not waits:
        return
    still = []
    for wait in waits:
        if wait.entity is None or wait.entity == entity:
            wait.done = True
            wait.result = data
        else:
            still.append(wait)
    _waiting_events[name] = still


def _cancel_module(module):
    for task in _tasks:
        if task._module == module:
            _cancel(task)


def _running():
    return len([task for task in _tasks if not task.done])
