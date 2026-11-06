# Beetles walk until the ground ends or something is in the way, then turn:
# two rays a step ahead, looking only for the level (category 2: the ground,
# walls and beds). Moving the transform moves their sensor's body.
from kin import component, physics, vec2, Transform2D

LEVEL = 2  # the collider category of the ground, walls and beds


@component
class Beetle:
    speed: float = 70.0   # across, + is right

    def update(self, dt):
        t = self.entity[Transform2D]
        step = 14.0 if self.speed > 0 else -14.0
        ahead = vec2(t.pos.x + step, t.pos.y)
        ground = physics.raycast(ahead, ahead + vec2(0, 16), mask=LEVEL)
        blocked = physics.raycast(t.pos, ahead, mask=LEVEL)
        if ground is None or blocked is not None:
            self.speed = self.speed * -1
        t.pos = vec2(t.pos.x + self.speed * dt, t.pos.y)
