# Seeds: a sensor each, and a little fade when one is picked.
from kin import component, run, emit, world, vec2, color, Transform2D, RectRenderer, \
    PhysicsBodyComponent, PhysicsColliderComponent


@component
class Seed:
    pass


def plant(at):
    e = world.create("seed")
    e.add(Transform2D, pos=at)
    e.add(RectRenderer, size=vec2(10, 10), offset=vec2(-5, -5), color=color(250, 240, 90, 255))
    e.add(PhysicsBodyComponent, type="static")
    e.add(PhysicsColliderComponent, shape="circle", radius=8.0, sensor=True)
    e.add(Seed)
    return e


def pick(e):
    e.remove(Seed)  # picked once
    emit("seed_picked")
    e.run(fade(e))


def fade(e):
    yield e.tween("Transform2D.scale", vec2(2.5, 2.5), 0.25, ease="ease_out")
    yield e.tween("RectRenderer.color", color(250, 240, 90, 0), 0.2)
    e.destroy()
