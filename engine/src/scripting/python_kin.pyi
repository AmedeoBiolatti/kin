# Type stubs of kin's Python module (docs/python_scripting.md), for editors
# (VS Code with Pylance/pyright): PythonScripts::write_stubs writes this, then
# a class for each component kin describes. Not read by kin itself.
from typing import Any, Callable, Generator, Iterator, Optional, TypeVar, Union, overload

T = TypeVar("T")


class vec2:
    x: float
    y: float
    ZERO: "vec2"
    ONE: "vec2"
    LEFT: "vec2"
    RIGHT: "vec2"
    UP: "vec2"
    DOWN: "vec2"
    def __init__(self, x: float, y: float) -> None: ...
    def __add__(self, other: "vec2") -> "vec2": ...
    def __sub__(self, other: "vec2") -> "vec2": ...
    def __mul__(self, other: Union[float, "vec2"]) -> "vec2": ...
    def __rmul__(self, other: float) -> "vec2": ...
    def __truediv__(self, other: Union[float, "vec2"]) -> "vec2": ...
    def dot(self, other: "vec2") -> float: ...
    def length(self) -> float: ...
    def length_squared(self) -> float: ...
    def normalize(self) -> "vec2": ...
    def rotate(self, radians: float) -> "vec2": ...
    def with_x(self, x: float) -> "vec2": ...
    def with_y(self, y: float) -> "vec2": ...


class vec2i:
    x: int
    y: int
    def __init__(self, x: int, y: int) -> None: ...
    def __add__(self, other: "vec2i") -> "vec2i": ...
    def __sub__(self, other: "vec2i") -> "vec2i": ...


class color:
    r: int
    g: int
    b: int
    a: int
    def __init__(self, r: int, g: int, b: int, a: int = 255) -> None: ...


class Entity:
    """An entity of the world. Equal to another when it is the same entity."""
    id: int
    alive: bool
    name: str
    parent: Optional["Entity"]
    children: list["Entity"]
    def child(self, name: str) -> Optional["Entity"]: ...
    def destroy(self) -> None: ...
    def has(self, component: Union[type, str]) -> bool: ...
    def add(self, component: type[T], **fields: Any) -> T:
        """The view, the component added (with those fields) when missing, else those fields set."""
    def remove(self, component: Union[type, str]) -> None: ...
    def get(self, component: type[T]) -> Optional[T]: ...
    def __getitem__(self, component: type[T]) -> T:
        """Its view (KeyError when it has none)."""
    def emit(self, name: str, data: Any = None) -> None:
        """An event of this entity, for @on(name) handlers and event(name) waits."""
    def run(self, task: Generator[Any, None, Any]) -> "Task":
        """A task belonging to this entity: cancelled when it is destroyed."""
    def tween(self, field: str, to: Any, seconds: float, ease: str = "ease_out", delay: float = 0.0,
              repeats: int = 0, yoyo: bool = False, relative: bool = False) -> "Tween":
        """Animates "Component.field" to `to`; yield it to wait for its end."""


class View:
    """What e[Component] gives: the entity's component, its fields read and written in place."""
    entity: Entity


class Awaitable:
    done: bool
    result: Any
    def __iter__(self) -> Iterator[Any]: ...


class Task(Awaitable):
    cancelled: bool
    error: Optional[str]
    def cancel(self) -> None: ...


class Tween(Awaitable):
    def stop(self) -> None: ...


class Cancelled(Exception): ...
class TaskFailed(Exception): ...
class SaveError(Exception): ...


class Hit:
    """What a raycast or a cast ran into."""
    entity: Optional[Entity]
    point: vec2
    normal: vec2
    fraction: float


class Slot:
    """A save slot: updated in Unix seconds, played in seconds."""
    id: str
    title: str
    summary: str
    updated: int
    played: int


def field(default: Any = None, min: Optional[float] = None, max: Optional[float] = None, label: str = "",
          description: str = "", units: str = "", hidden: bool = False, readonly: bool = False,
          choices: Optional[list[str]] = None, asset_type: str = "") -> Any:
    """A component field with options: `hp: int = field(100, min=0)`."""

def component(cls: type[T]) -> type[T]:
    """The class becomes a kin component: its annotated fields are the component's."""

def system(*components: Union[type, str], without: Any = None) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    """The function runs each frame for every entity with the components: f(dt, *views)."""

def on(name: str, *components: Union[type, str]) -> Callable[[Callable[[Optional[Entity], Any], Any]], Callable[..., Any]]:
    """The function runs for each such event (of an entity with the components): f(entity, data)."""

def emit(name: str, data: Any = None) -> None: ...
def run(task: Generator[Any, None, Any]) -> Task: ...
def query(*components: Union[type, str], without: Any = None) -> list[Any]: ...

def wait(seconds: float) -> Awaitable: ...
def frames(count: int) -> Awaitable: ...
def until(condition: Callable[[], bool]) -> Generator[None, None, None]: ...
def event(name: str, entity: Optional[Entity] = None) -> Awaitable: ...
def sync(*items: Any) -> Awaitable: ...
def race(*items: Any) -> Awaitable: ...


class _World:
    def find(self, name: str) -> Optional[Entity]: ...
    def create(self, name: str = "") -> Entity: ...
    def spawn(self, prefab: str, at: Union[vec2, tuple[float, float], None] = None) -> Entity: ...
    def single(self, component: type[T]) -> Optional[T]: ...
    def query(self, *components: Union[type, str], without: Any = None) -> list[Any]: ...

class _Input:
    def held(self, action: str) -> bool: ...
    def pressed(self, action: str) -> bool: ...
    def released(self, action: str) -> bool: ...
    def axis(self, name: str) -> float: ...
    def mouse(self) -> vec2: ...

class _Time:
    dt: float
    now: float
    frame: int

Point = Union[vec2, tuple[float, float]]

class _Physics:
    """What is where, in the game's physics (positions in the world). `mask`: the
    kinds of collider to find (bits); `sensors`: find sensors too; `ignore`: an
    entity whose body is passed over (the one asking, say)."""
    def raycast(self, start: Point, end: Point, mask: int = 0xFFFF, sensors: bool = False,
                ignore: Optional[Entity] = None) -> Optional[Hit]:
        """The nearest thing on the line from start to end."""
    def cast(self, start: Point, end: Point, radius: float = 0.0, size: Optional[Point] = None, mask: int = 0xFFFF,
             sensors: bool = False, ignore: Optional[Entity] = None) -> Optional[Hit]:
        """The first thing a circle (radius) or a box (size: width, height), centred on the point and not
        turning, runs into going from start to end."""
    def overlap(self, at: Point, radius: float = 0.0, size: Optional[Point] = None, mask: int = 0xFFFF,
                sensors: bool = False, ignore: Optional[Entity] = None) -> list[Entity]:
        """The entities at a point, or in a circle (radius) or a box (size) centred there."""

class _Scene:
    def go(self, path: str = "", arrive: str = "", transition: Optional[str] = "fade", seconds: float = 0.5) -> None:
        """Asks the game to go to a scene file of the content (empty: a place of this one), arriving at the
        entity named `arrive`. transition: "fade", "crossfade", "dissolve", "pixelate", "wipe", "iris",
        or "none"/None."""
    def going(self) -> bool:
        """Whether a scene is asked for and not yet gone to."""

class _Save:
    """The game's saves: slots, and its own values, kept from scene to scene and in the slots."""
    def write(self, slot: str, title: str = "", summary: str = "") -> None:
        """The game written to the slot (SaveError when it can't be)."""
    def read(self, slot: str) -> None:
        """Asks the game to read the slot, after this update (SaveError when there is none)."""
    def exists(self, slot: str) -> bool: ...
    def delete(self, slot: str) -> None: ...
    def slots(self) -> list[Slot]:
        """The slots, the newest first."""
    def get(self, name: str, default: Any = None) -> Any:
        """A value of the game's (a whole number comes back an int)."""
    def set(self, name: str, value: Any) -> None:
        """None, True/False, numbers, text, and lists and dicts of them; None takes it away."""

world: _World
input: _Input
time: _Time
physics: _Physics
scene: _Scene
save: _Save

# The components kin describes:
