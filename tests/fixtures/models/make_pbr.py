"""Makes pbr.glb, kin-render3d's test model for materials (python3 make_pbr.py):
a quad with every metallic-roughness field set (factors, the maps, a cut-off
alpha), and two quads side by side, lit the same way: one with a flat normal
map (128, 128, 255), one with none, which must look alike if the map is read
as the numbers it holds rather than as colours."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools", "models"))
from glb import Glb, quad  # noqa: E402

model = Glb()
flat = model.texture(1, 1, [128, 128, 255, 255])
rough = model.texture(1, 1, [255, 90, 200, 255])
occlusion = model.texture(1, 1, [128, 255, 255, 255])
glow = model.texture(1, 1, [255, 160, 0, 255])
leaf = model.texture(2, 1, [30, 160, 40, 0, 30, 160, 40, 255], nearest=True)
everything = model.material("everything", color=(0.5, 0.25, 0.125, 1), texture=leaf, metallic=0.75, roughness=0.5,
                            metallic_roughness_texture=rough, normal_texture=flat, normal_scale=0.5,
                            occlusion_texture=occlusion, occlusion_strength=0.25, emissive=(0.5, 0.2, 0.1),
                            emissive_texture=glow, alpha_cutoff=0.375, double_sided=True)
mapped = model.material("mapped", color=(0.8, 0.8, 0.8, 1), normal_texture=flat)
plain = model.material("plain", color=(0.8, 0.8, 0.8, 1))
model.node("everything", mesh=model.mesh("everything", *quad(1, 1), material=everything))
model.node("mapped", mesh=model.mesh("mapped", *quad(1, 1), material=mapped), translation=(-2, 0, 0))
model.node("plain", mesh=model.mesh("plain", *quad(1, 1), material=plain), translation=(2, 0, 0))
model.save(os.path.join(os.path.dirname(__file__), "pbr.glb"))
