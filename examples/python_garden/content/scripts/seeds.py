# Seeds (a prefab, marked Saved: once picked, gone for good, in this scene
# and in the saves). Picking one leaves a puff that fades.
from kin import component, emit, world, vec2, color, Transform2D, RectRenderer


@component
class Seed:
    pass


def pick(e):
    at = e[Transform2D].pos
    e.destroy()
    emit("seed_picked")
    puff = world.create("puff")
    puff.add(Transform2D, pos=at)
    puff.add(RectRenderer, size=vec2(10, 10), offset=vec2(-5, -5), color=color(250, 240, 90, 255))
    puff.run(fade(puff))


def fade(e):
    yield e.tween("Transform2D.scale", vec2(2.5, 2.5), 0.25, ease="ease_out")
    yield e.tween("RectRenderer.color", color(250, 240, 90, 0), 0.2)
    e.destroy()
