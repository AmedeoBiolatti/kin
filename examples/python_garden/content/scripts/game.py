# The game's rules. What lasts the whole game (seeds picked, lives) is in
# save's values, kept from scene to scene and in the save slots; what a scene
# shows (its welcome, the message) is its "game" entity's Game component.
from kin import component, on, system, emit, wait, world, input, save, SaveError

TOTAL = 12  # seeds: 9 in the garden, 3 in the shed


@component
class Game:
    welcome: str = ""     # said as the scene begins
    message: str = ""     # the line across the screen, when there is one

    def start(self):
        if self.welcome:
            self.entity.run(say(self.welcome, 4))


def game():
    return world.single(Game)


def seeds():
    return save.get("seeds", 0)


def lives():
    return save.get("lives", 3)


def over():
    return lives() <= 0 or seeds() == TOTAL


def say(text, seconds=1.5):
    """A message for a while: a task, so a newer one simply replaces it."""
    g = game()
    g.message = text
    yield wait(seconds)
    if g.entity.alive and g.message == text:
        g.message = ""


def tell(text, seconds=1.5):
    game().entity.run(say(text, seconds))


@on("seed_picked")
def picked(e, data):
    save.set("seeds", seeds() + 1)
    if seeds() == TOTAL:
        tell("The garden is planted!  R plays again", 1000)


@on("gardener_lost")
def lost(e, data):
    import gardener
    save.set("lives", lives() - 1)
    if lives() <= 0:
        e.destroy()
        tell("The beetles won.  R plays again", 1000)
    else:
        tell("Ouch!  " + str(lives()) + " left")
        e.run(gardener.recover(e))


@system(Game)
def keys(dt, g):
    if input.pressed("save"):
        emit("quicksave")
    if input.pressed("load"):
        emit("quickload")


@on("quicksave")
def quicksave(e, data):
    if over():
        tell("The game is over: R plays again")
        return
    try:
        save.write("quick", title="Garden", summary=str(seeds()) + "/" + str(TOTAL) + " seeds, " +
                   str(lives()) + " lives")
        tell("Saved  (F9 reads it back)")
    except SaveError as error:
        tell("Not saved: " + str(error), 3)


@on("quickload")
def quickload(e, data):
    if save.exists("quick"):
        save.read("quick")  # the game reads it after this update, then says "loaded"
    else:
        tell("Nothing saved yet: F5 saves")


@on("loaded")
def loaded(e, data):
    tell("Read back: " + str(seeds()) + "/" + str(TOTAL) + " seeds")
