"""Makes Signal Breach's models into examples/content/models (python3
tools/models/make_breach_models.py): gun.glb, the player's gun as the overlay
camera sees it (along -z, the grip below), and crate.glb, a one-metre crate
on the deck. Textures are made here too. Edit and run again while the game
runs: it loads them again. (Kept out of the content folder, which ships.)"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glb import Glb, box, cylinder  # noqa: E402

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples", "content", "models")


def metal(width, height):
    """Brushed metal: grey streaks along u, a darker band each 16 texels."""
    rng = random.Random(7)
    rows = []
    for y in range(height):
        base = 120 + rng.randint(-10, 10) - (25 if y % 16 < 2 else 0)
        for x in range(width):
            v = max(0, min(255, base + rng.randint(-6, 6)))
            rows += [v, v + 4, v + 10, 255]
    return rows


def planks(width, height):
    """Wooden boards with a frame round the edge."""
    rng = random.Random(3)
    rows = []
    for y in range(height):
        board = y // (height // 4)
        shade = 140 + board * 9 + rng.randint(-4, 4)
        for x in range(width):
            edge = x < 4 or y < 4 or x >= width - 4 or y >= height - 4
            seam = y % (height // 4) == 0
            grain = rng.randint(-8, 8)
            v = shade - 50 if edge else shade - 35 if seam else shade + grain
            rows += [max(0, v), max(0, int(v * 0.7)), max(0, int(v * 0.42)), 255]
    return rows


gun = Glb()
steel = gun.material("steel", texture=gun.texture(64, 64, metal(64, 64)))
dark = gun.material("grip", color=(0.05, 0.06, 0.07, 1))
glow = gun.material("sight", color=(0.3, 1.0, 0.9, 1), emissive=(1, 1, 1), unlit=True)
body = gun.node("body", mesh=gun.mesh("body", *box((0.08, 0.1, 0.36)), material=steel), root=False)
grip = gun.node("grip", mesh=gun.mesh("grip", *box((0.05, 0.12, 0.07)), material=dark),
                translation=(0, -0.09, 0.1), rotation=(-0.1305, 0, 0, 0.9914), root=False)
barrel = gun.node("barrel", mesh=gun.mesh("barrel", *cylinder(0.022, 0.3), material=steel),
                  translation=(0, 0.02, -0.32), root=False)
sight = gun.node("sight", mesh=gun.mesh("sight", *box((0.02, 0.02, 0.02)), material=glow),
                 translation=(0, 0.065, -0.1), root=False)
gun.node("gun", children=[body, grip, barrel, sight])
gun.save(os.path.join(here, "gun.glb"))

crate = Glb()
wood = crate.material("wood", texture=crate.texture(64, 64, planks(64, 64)))
crate.node("crate", mesh=crate.mesh("crate", *box((0.96, 0.96, 0.96)), material=wood), translation=(0, 0.48, 0))
crate.save(os.path.join(here, "crate.glb"))
