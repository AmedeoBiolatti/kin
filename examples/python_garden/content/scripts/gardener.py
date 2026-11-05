# The gardener: kin's character controller does the walking and the falling;
# this says where to go, from the keys or, with Autoplay, by itself.
from kin import component, on, system, wait, world, input, vec2, color, Transform2D, RectRenderer, \
    PhysicsBodyComponent, PhysicsColliderComponent, PhysicsContactQueue, CharacterController2D

SPEED = 220.0
JUMP = 760.0


@component
class Gardener:
    home: vec2 = vec2(480, 460)


@component
class Unhurt:
    """Just hit: beetles go through it for a while."""


def arrive(at):
    e = world.create("gardener")
    e.add(Transform2D, pos=at)
    e.add(RectRenderer, size=vec2(18, 30), offset=vec2(-9, -15), color=color(240, 210, 120, 255))
    e.add(PhysicsBodyComponent)
    e.add(PhysicsColliderComponent, shape="box", half_extents=vec2(9, 15))
    e.add(PhysicsContactQueue)
    e.add(CharacterController2D, gravity=1400.0, snap=4.0)
    e.add(Gardener, home=at)
    return e


def steer(e):
    """(-1 to 1 across, jump, drop): the keys, or the autoplayer's guess."""
    import game
    if game.game().entity.has(game.Autoplay):
        return autopilot(e)
    across = input.axis("move")
    return across, input.pressed("jump"), input.held("down")


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


def recover(e):
    """Back home, blinking, and safe from beetles for a while."""
    e.add(Unhurt)
    e[Transform2D].pos = e[Gardener].home
    e[CharacterController2D].velocity = vec2(0, 0)
    for _ in range(6):
        yield e.tween("RectRenderer.color", color(240, 210, 120, 60), 0.12, ease="linear")
        yield e.tween("RectRenderer.color", color(240, 210, 120, 255), 0.12, ease="linear")
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
