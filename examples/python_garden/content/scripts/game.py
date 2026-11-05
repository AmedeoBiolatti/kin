# The game's own state, on one entity ("game"): what the HUD shows and the
# rules read. Everything that must survive a reload is here, in components.
from kin import component, on, run, emit, wait, world
import garden
import gardener
import seeds
import beetles


@component
class Game:
    seeds: int = 0        # picked so far
    total: int = 0        # in the garden
    lives: int = 3
    over: bool = False
    message: str = ""     # the line across the screen, when there is one


@component
class Autoplay:
    """On the game entity, the gardener plays itself (the headless smoke test)."""


def game():
    return world.single(Game)


def say(text, seconds):
    """A message for a while: a task, so a newer one simply replaces it."""
    game().message = text
    yield wait(seconds)
    if game().message == text:
        game().message = ""


def fill():
    for e in world.query(seeds.Seed) + world.query(beetles.Beetle) + world.query(gardener.Gardener):
        e.entity.destroy()
    g = game()
    g.seeds = 0
    g.lives = 3
    g.over = False
    g.total = len(garden.SEEDS)
    for at in garden.SEEDS:
        seeds.plant(at)
    for at, walk in garden.BEETLES:
        beetles.hatch(at, walk)
    gardener.arrive(garden.START)
    run(say("Pick every seed!  Arrows or A/D, W or Space to jump, S to drop", 4))


@on("start")
def start(e, data):
    garden.build()
    world.create("game").add(Game)
    fill()


@on("autoplay")
def autoplay(e, data):
    game().entity.add(Autoplay)


@on("restart")
def restart(e, data):
    fill()


@on("seed_picked")
def picked(e, data):
    g = game()
    g.seeds += 1
    if g.seeds == g.total:
        g.over = True
        run(say("The garden is planted!  R plays again", 1000))


@on("gardener_lost")
def lost(e, data):
    g = game()
    g.lives -= 1
    if g.lives <= 0:
        g.over = True
        e.destroy()
        run(say("The beetles won.  R plays again", 1000))
    else:
        run(say("Ouch!  " + str(g.lives) + " left", 1.5))
        e.run(gardener.recover(e))
