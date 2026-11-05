# What the player reads: two texts, kept up to date by a system from the
# Game component. The C++ host draws nothing of its own.
from kin import on, system, world, vec2, color, Transform2D, TextRenderer
from game import Game
import gardener


def label(name, at, size):
    e = world.create(name)
    e.add(Transform2D, pos=at)
    e.add(TextRenderer, text="", size=size, pivot=vec2(0, 0), color=color(255, 255, 255, 255), outline=2.0,
          outline_color=color(40, 50, 60, 255))
    return e


@on("start")
def make(e, data):
    label("score", vec2(16, 12), 22.0)
    label("message", vec2(16, 44), 22.0)


@system(Game)
def show(dt, g):
    score = world.find("score")
    if score is not None:
        score[TextRenderer].text = "Seeds " + str(g.seeds) + "/" + str(g.total) + "    Lives " + str(g.lives)
    message = world.find("message")
    if message is not None:
        message[TextRenderer].text = g.message


