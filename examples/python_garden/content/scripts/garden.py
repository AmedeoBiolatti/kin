# The garden itself: ground, walls and beds, made of kin's components. A
# scene file could hold the same; built here to show composing them.
from kin import world, vec2, color, Transform2D, RectRenderer, PhysicsBodyComponent, PhysicsColliderComponent

SOIL = color(92, 64, 44, 255)
BED = color(120, 156, 72, 255)

# Where the seeds grow and the beetles walk: (x, y, half the walk's width).
SEEDS = [vec2(140, 380), vec2(260, 380), vec2(700, 380), vec2(820, 380), vec2(430, 300), vec2(530, 300),
         vec2(480, 170), vec2(60, 480), vec2(900, 480)]
BEETLES = [(vec2(480, 494), 300.0), (vec2(480, 316), 70.0)]
START = vec2(480, 460)


def block(name, centre, half, tint, one_sided=False):
    e = world.create(name)
    e.add(Transform2D, pos=centre)
    e.add(RectRenderer, size=half * 2, offset=half * -1, color=tint)
    e.add(PhysicsBodyComponent, type="static")
    e.add(PhysicsColliderComponent, shape="box", half_extents=half, one_sided=one_sided)
    return e


def build():
    block("ground", vec2(480, 520), vec2(480, 20), SOIL)
    block("left wall", vec2(-10, 270), vec2(10, 300), SOIL)
    block("right wall", vec2(970, 270), vec2(10, 300), SOIL)
    block("low bed west", vec2(200, 410), vec2(80, 8), BED, one_sided=True)
    block("low bed east", vec2(760, 410), vec2(80, 8), BED, one_sided=True)
    block("middle bed", vec2(480, 330), vec2(90, 8), BED, one_sided=True)
    block("high bed", vec2(480, 200), vec2(60, 8), BED, one_sided=True)
