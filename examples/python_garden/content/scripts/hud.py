# What the player reads: the HUD prefab's two texts, kept up to date from the
# game's values and the scene's message. The C++ host draws nothing of its own.
from kin import system, world, TextRenderer
import game


@system(game.Game)
def show(dt, g):
    score = world.find("score")
    if score is not None:
        score[TextRenderer].text = "Seeds " + str(game.seeds()) + "/" + str(game.TOTAL) + "    Lives " + \
            str(max(game.lives(), 0)) + "    F5 save, F9 load"
    message = world.find("message")
    if message is not None:
        message[TextRenderer].text = g.message
