# The gardener (a prefab in each scene): kin's character controller does the
# walking and the falling, kin's scene flow the doors; this says where to go,
# from the keys or, with the game's "autoplay" value, by itself.
from kin import component, on, system, until, world, input, physics, save, vec2, color, Transform2D, \
    RectRenderer, CharacterController2D

SPEED = 220.0
JUMP = 760.0


@component
class Gardener:
    home: vec2 = vec2(0, 0)   # where it came into the scene: back there when hurt

    def start(self):
        self.home = self.entity[Transform2D].pos


@component
class Unhurt:
    """Just hit: beetles go through it for a while."""


def steer(e):
    """(-1 to 1 across, jump, drop): the keys, or the autoplayer's guess."""
    if save.get("autoplay", False):
        return autopilot(e)
    return input.axis("move"), input.pressed("jump"), input.held("down")


def autopilot(e):
    import seeds
    me = e[Transform2D].pos
    left = [s.entity[Transform2D].pos for s in world.query(seeds.Seed)]
    if not left:
        return 0.0, False, False
    target = left[0]
    for p in left:
        if (p - me).length() < (target - me).length():
            target = p
    across = 0.0
    if target.x > me.x + 6:
        across = 1.0
    elif target.x < me.x - 6:
        across = -1.0
    above = target.y < me.y - 40
    return across, above and abs(target.x - me.x) < 120, target.y > me.y + 40


@system(CharacterController2D, Gardener)
def move(dt, body, g):
    across, jump, drop = steer(g.entity)
    v = body.velocity
    v = vec2(across * SPEED, v.y)
    if jump and body.on_floor:
        v = vec2(v.x, -JUMP)
    body.velocity = v
    if drop and body.on_floor:
        body.drop_through = 0.25


def beetle_near(at):
    import beetles
    return [b for b in physics.overlap(at, radius=48, sensors=True) if b.has(beetles.Beetle)]


def faded(e, alpha):
    c = e[RectRenderer].color
    return color(c.r, c.g, c.b, alpha)


def recover(e):
    """Back home once no beetle is there, blinking, and safe for a while."""
    e.add(Unhurt)
    home = e[Gardener].home
    yield until(lambda: not beetle_near(home))
    e[Transform2D].pos = home
    e[CharacterController2D].velocity = vec2(0, 0)
    for _ in range(6):
        yield e.tween("RectRenderer.color", faded(e, 60), 0.12, ease="linear")
        yield e.tween("RectRenderer.color", faded(e, 255), 0.12, ease="linear")
    e.remove(Unhurt)


@on("contact", Gardener)
def touched(e, other):
    import seeds
    import beetles
    if other is None or not other.alive:
        return
    if other.has(seeds.Seed):
        seeds.pick(other)
    elif other.has(beetles.Beetle) and not e.has(Unhurt):
        e.emit("gardener_lost")
