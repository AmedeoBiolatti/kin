# Beetles walk back and forth over their bed: a component with an update,
# moving a sensor (kin sends its body along when the transform is written).
from kin import component, world, vec2, color, Transform2D, RectRenderer, PhysicsBodyComponent, \
    PhysicsColliderComponent


@component
class Beetle:
    centre: vec2 = vec2(0, 0)
    walk: float = 100.0    # how far either side it goes
    speed: float = 70.0

    def update(self, dt):
        t = self.entity[Transform2D]
        x = t.pos.x + self.speed * dt
        if abs(x - self.centre.x) > self.walk:
            self.speed = -self.speed
        t.pos = vec2(x, t.pos.y)


def hatch(at, walk):
    e = world.create("beetle")
    e.add(Transform2D, pos=at)
    e.add(RectRenderer, size=vec2(22, 12), offset=vec2(-11, -6), color=color(60, 40, 90, 255))
    e.add(PhysicsBodyComponent, type="static", sync_to_physics=True)
    e.add(PhysicsColliderComponent, shape="box", half_extents=vec2(11, 6), sensor=True)
    e.add(Beetle, centre=at, walk=walk)
    return e
