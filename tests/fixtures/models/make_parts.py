"""Makes parts.glb, kin-render3d's test model (python3 make_parts.py):
a red box at the origin; a child node 1 m up, scaled 2, holding an unlit
quad with a 2 x 2 checker (red, green / blue, white) and no normals; an
alpha-blended, double-sided pane beside it."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools", "models"))
from glb import Glb, box, quad  # noqa: E402

model = Glb()
red = model.material("red", color=(1, 0, 0, 1))
checker = model.texture(2, 2, [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255], nearest=True)
panel_material = model.material("checker", texture=checker, unlit=True)
pane_material = model.material("glass", color=(0.2, 0.4, 1.0, 0.5), blend=True, double_sided=True)
panel = model.node("panel", mesh=model.mesh("panel", *quad(1, 1), material=panel_material),
                   translation=(0, 1, 0), scale=(2, 2, 2), root=False)
model.node("base", mesh=model.mesh("box", *box((1, 1, 1)), material=red), children=[panel])
model.node("pane", mesh=model.mesh("pane", *quad(1, 1), material=pane_material), translation=(3, 0, 0))
model.save(os.path.join(os.path.dirname(__file__), "parts.glb"))
