"""A kin scene snapshot (write_scene_snapshot, kin/render3d/snapshot3d.hpp) rendered by Blender's Cycles.

    blender -b --factory-startup --python tools/cycles/render_snapshot.py -- <snapshot dir> --out <dir>
        [--samples N] [--seed N] [--denoise]

Writes <out>/cycles.exr (linear, RGBA: alpha 0 where kin's picture is not compared: a portal) and <out>/cycles.png
(the same through the view's exposure and tonemap, as kin shows it). tools/cycles/compare.py runs it.

How kin's scene becomes Cycles' (the lights as tools/reference/blender_truth.py maps them):

- objects: each mesh copy, its vertices in its own space placed by its transform (kin's y up to Blender's z up);
  baked materials read the snapshot's pages (base colour times the vertex colour, alpha, world normal, roughness in
  G and metallic in B of the ORM page, emission); the others their constants; occlusion maps are left out (Cycles
  traces the occlusion);
- `water` a Principled BSDF with `transmission` of light through (IOR 1.33), casting no shadow (its murk is not
  written), `holdout` a hole (not compared);
- `unlit` an emission of its colour, unseen by diffuse rays (kin's unlit surfaces light nothing);
- the sun and the fill Sun lamps of strength pi * max(c) (the sun's disc as wide as the sky's), point and spot lights
  lamps of power 4 pi^2 * max(c) (a white surface facing one at a metre then shows c, as kin's; kin fades them to
  nothing at their radius, Cycles does not), as big as their size;
- the world the sky's panorama (sky.hdr, when the sky lights the scene) or the ambient (above, and the ground
  ambient below).
"""
import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reference"))
import blender_truth as bt  # noqa: E402


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opts = {"samples": 256, "seed": 0, "denoise": False, "out": None}
    rest = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--samples", "--seed"):
            opts[a[2:]] = argv[i + 1]
            i += 2
        elif a == "--denoise":
            opts["denoise"] = True
            i += 1
        else:
            rest.append(a)
            i += 1
    opts["snapshot"] = rest[0]
    opts["out"] = opts["out"] or os.path.join(rest[0], "cycles")
    return opts


# kin's world (y up) to Blender's (z up).
KIN_TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))


def kin_matrix(m):
    """kin's Mat4f (column-major, 16 floats) as a mathutils Matrix."""
    return Matrix([[m[c * 4 + r] for c in range(4)] for r in range(4)])


def socket(sockets, name, kind="RGBA"):
    """A node's socket by name and type (a Mix node has a Float, a Vector and a Color "A")."""
    return next(s for s in sockets if s.name == name and s.type == kind)


def aces(x):
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)


def srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


class Materials:
    def __init__(self, doc, folder):
        self.doc, self.folder = doc, folder
        self.cache, self.images = {}, {}

    def image(self, name, colour):
        if name not in self.images:
            img = bpy.data.images.load(os.path.join(self.folder, "pages", name))
            img.colorspace_settings.name = "sRGB" if colour else "Non-Color"
            self.images[name] = img
        return self.images[name]

    def get(self, o, page):
        key = (o["kind"], page if o["baked"] else None, o["unlit"], o["blend"], o["emission_from_base"],
               None if o["baked"] else (tuple(o["color"]), o["alpha"], o["metallic"], o["roughness"],
                                         tuple(o["emission"])),
               o.get("transmission"))
        if key not in self.cache:
            self.cache[key] = self.make(o, page)
        return self.cache[key]

    def make(self, o, page):
        mat = bpy.data.materials.new(f"kin_{len(self.cache)}")
        mat.use_nodes = True
        nodes, links = mat.node_tree.nodes, mat.node_tree.links
        out = nodes["Material Output"]
        bsdf = nodes["Principled BSDF"]
        if o["kind"] == "holdout":
            nodes.remove(bsdf)
            links.new(nodes.new("ShaderNodeHoldout").outputs[0], out.inputs["Surface"])
            return mat
        vcol = nodes.new("ShaderNodeVertexColor")
        vcol.layer_name = "kin_color"
        baked = o["baked"]

        def tex(channel, colour):
            node = nodes.new("ShaderNodeTexImage")
            node.image = self.image(f"page{page}_{channel}.png", colour)
            node.interpolation = "Linear"
            node.extension = "EXTEND"
            return node

        def times(a, b):
            m = nodes.new("ShaderNodeMix")
            m.data_type = "RGBA"
            m.blend_type = "MULTIPLY"
            m.inputs["Factor"].default_value = 1.0
            links.new(a, socket(m.inputs, "A"))
            links.new(b, socket(m.inputs, "B"))
            return socket(m.outputs, "Result")

        def constant(rgb):
            node = nodes.new("ShaderNodeRGB")
            node.outputs[0].default_value = (*rgb, 1.0)
            return node.outputs[0]

        base = times(tex("base", True).outputs["Color"] if baked else constant(o["color"]), vcol.outputs["Color"])
        emission = None
        if baked and page in self.doc.get("emission_pages", []):
            node = nodes.new("ShaderNodeTexImage")
            node.image = self.image(f"page{page}_emission.hdr", False)
            node.image.colorspace_settings.name = "Linear Rec.709"
            node.interpolation = "Linear"
            emission = node.outputs["Color"]
        elif not baked and max(o["emission"]) > 0:
            emission = constant(o["emission"])
        if emission is not None and o["emission_from_base"]:
            emission = times(emission, vcol.outputs["Color"])

        if o["unlit"]:
            # Its colour as it is (baked: the emission page holds the base colour plus its emission), lighting nothing.
            nodes.remove(bsdf)
            em = nodes.new("ShaderNodeEmission")
            colour = emission if baked and emission is not None else base
            if not baked and emission is not None:
                add = nodes.new("ShaderNodeMix")
                add.data_type = "RGBA"
                add.blend_type = "ADD"
                add.inputs["Factor"].default_value = 1.0
                links.new(base, socket(add.inputs, "A"))
                links.new(emission, socket(add.inputs, "B"))
                colour = socket(add.outputs, "Result")
            links.new(colour, em.inputs["Color"])
            links.new(em.outputs[0], out.inputs["Surface"])
            return mat

        links.new(base, bsdf.inputs["Base Color"])
        if baked:
            orm = nodes.new("ShaderNodeSeparateColor")
            links.new(tex("orm", False).outputs["Color"], orm.inputs[0])
            links.new(orm.outputs["Green"], bsdf.inputs["Roughness"])
            links.new(orm.outputs["Blue"], bsdf.inputs["Metallic"])
            # The world normal (kin's axes, n * 0.5 + 0.5) as Blender's: (x, -z, y).
            n = nodes.new("ShaderNodeVectorMath")
            n.operation = "MULTIPLY_ADD"
            links.new(tex("normal", False).outputs["Color"], n.inputs[0])
            n.inputs[1].default_value = (2, 2, 2)
            n.inputs[2].default_value = (-1, -1, -1)
            split = nodes.new("ShaderNodeSeparateXYZ")
            links.new(n.outputs[0], split.inputs[0])
            neg = nodes.new("ShaderNodeMath")
            neg.operation = "MULTIPLY"
            neg.inputs[1].default_value = -1.0
            links.new(split.outputs["Z"], neg.inputs[0])
            join = nodes.new("ShaderNodeCombineXYZ")
            links.new(split.outputs["X"], join.inputs["X"])
            links.new(neg.outputs[0], join.inputs["Y"])
            links.new(split.outputs["Y"], join.inputs["Z"])
            norm = nodes.new("ShaderNodeVectorMath")
            norm.operation = "NORMALIZE"
            links.new(join.outputs[0], norm.inputs[0])
            links.new(norm.outputs[0], bsdf.inputs["Normal"])
        else:
            bsdf.inputs["Roughness"].default_value = o["roughness"]
            bsdf.inputs["Metallic"].default_value = o["metallic"]
        if emission is not None:
            links.new(emission, bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 1.0
        if o["kind"] == "water":
            bsdf.inputs["Transmission Weight"].default_value = o.get("transmission", 1.0)
            bsdf.inputs["IOR"].default_value = 1.33
        if o["blend"] != "opaque":
            alpha = tex("alpha", False).outputs["Color"] if baked else None
            if alpha is None:
                a = nodes.new("ShaderNodeValue")
                a.outputs[0].default_value = o["alpha"]
                alpha = a.outputs[0]
            if o["blend"] == "mask":
                cut = nodes.new("ShaderNodeMath")
                cut.operation = "GREATER_THAN"
                cut.inputs[1].default_value = o["alpha_cutoff"] - 1e-4
                links.new(alpha, cut.inputs[0])
                alpha = cut.outputs[0]
            links.new(alpha, bsdf.inputs["Alpha"])
        return mat


def add_object(o, index, materials, folder, meshes):
    data = np.fromfile(os.path.join(folder, o["mesh"]), dtype=np.float32).reshape(-1, 13)
    name = f"kin_{index}"
    key = o["mesh"]
    if key in meshes and not o["baked"]:
        mesh = meshes[key].copy()
    else:
        n = len(data)
        mesh = bpy.data.meshes.new(name)
        mesh.vertices.add(n)
        mesh.vertices.foreach_set("co", data[:, 0:3].ravel())
        mesh.loops.add(n)
        mesh.loops.foreach_set("vertex_index", np.arange(n, dtype=np.int32))
        mesh.polygons.add(n // 3)
        mesh.polygons.foreach_set("loop_start", np.arange(0, n, 3, dtype=np.int32))
        mesh.polygons.foreach_set("loop_total", np.full(n // 3, 3, dtype=np.int32))
        uv = mesh.uv_layers.new(name="UVMap")
        uvs = data[:, 6:8].copy()
        if o["baked"]:
            uvs[:, 1] = 1.0 - uvs[:, 1]  # (the pages' v runs down; Blender's up)
        uv.data.foreach_set("uv", uvs.ravel())
        col = mesh.color_attributes.new("kin_color", "FLOAT_COLOR", "CORNER")
        col.data.foreach_set("color", data[:, 8:12].ravel())
        mesh.update()
        mesh.validate(clean_customdata=False)
        mesh.normals_split_custom_set([tuple(v) for v in data[:, 3:6]])
        meshes.setdefault(key, mesh)
    mesh.materials.clear()
    if o["baked"]:
        pages = sorted(set(int(p) for p in data[::3, 12]))
        slot = {}
        for p in pages:
            slot[p] = len(mesh.materials)
            mesh.materials.append(materials.get(o, p))
        mesh.polygons.foreach_set("material_index", np.array([slot[int(p)] for p in data[::3, 12]], dtype=np.int32))
    else:
        mesh.materials.append(materials.get(o, None))
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.matrix_world = KIN_TO_BLENDER @ kin_matrix(o["transform"])
    if o["unlit"]:
        obj.visible_diffuse = False
    # (Water casts no shadow: a sun's or a lamp's light reaches what is under it, as kin lights a pool's floor; a path
    # tracer's direct light cannot pass through a refracting surface, and its caustics would take long to find.)
    if not o["casts_shadow"] or o["kind"] == "water":
        obj.visible_shadow = False
    return obj


def lamp(name, kind, position, color, size, direction=None):
    peak = max(color) if max(color) > 0 else 1.0
    data = bpy.data.lights.new(name, kind)
    data.energy = 4 * math.pi * math.pi * peak
    data.color = tuple(c / peak for c in color)
    data.shadow_soft_size = size
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = bt.blender(position)
    if direction is not None:
        obj.rotation_euler = bt.blender(direction).to_track_quat("-Z", "Y").to_euler()
    return data


def main():
    opts = args()
    folder = opts["snapshot"]
    doc = json.load(open(os.path.join(folder, "scene.json")))
    width, height = doc["size"]
    bt.clear()
    device = bt.setup_render(width, height, int(opts["samples"]), opts["seed"], opts["denoise"])
    scene = bpy.context.scene

    materials, meshes = Materials(doc, folder), {}
    for i, o in enumerate(doc["objects"]):
        add_object(o, i, materials, folder, meshes)

    if doc.get("sky") and doc.get("sky_lights_scene"):
        bt.environment(os.path.join(folder, doc["sky"]), doc.get("environment_intensity", 1.0), 0.0)
    elif doc.get("ground_ambient"):
        bt.world(tuple(doc["ambient"]), tuple(doc["ground_ambient"]))
    else:
        bt.world(tuple(doc["ambient"]))
    if max(doc["sun"]["color"]) > 0:
        sun = bt.sun("sun", doc["sun"]["direction"], doc["sun"]["color"], doc["sun"]["shadow"])
        sun.data.angle = math.radians(doc["sun"].get("angle", 0.0))
    if max(doc["fill"]["color"]) > 0:
        bt.sun("fill", doc["fill"]["direction"], doc["fill"]["color"], doc["fill"]["shadow"])
    for i, p in enumerate(doc["points"]):
        lamp(f"point_{i}", "POINT", p["position"], p["color"], p["size"])
    for i, p in enumerate(doc["spots"]):
        data = lamp(f"spot_{i}", "SPOT", p["position"], p["color"], p["size"], p["direction"])
        data.spot_size = math.radians(2 * p["outer"])
        data.spot_blend = max(0.0, min(1.0, (p["outer"] - p["inner"]) / max(p["outer"], 1e-3)))

    cam = doc["camera"]
    data = bpy.data.cameras.new("camera")
    data.sensor_fit = "VERTICAL"
    if cam["orthographic"]:
        data.type = "ORTHO"
        data.ortho_scale = cam["height"] * max(1.0, width / height)
    else:
        data.angle = math.radians(cam["fov"])
    data.clip_start = cam["near"]
    data.clip_end = min(cam["far"], 1e5)
    obj = bpy.data.objects.new("camera", data)
    scene.collection.objects.link(obj)
    obj.location = bt.blender(cam["position"])
    obj.rotation_euler = bt.blender(cam["forward"]).to_track_quat("-Z", "Y").to_euler()
    # (to_track_quat keeps world up; the camera's own up decides the roll)
    forward, up = bt.blender(cam["forward"]).normalized(), bt.blender(cam["up"]).normalized()
    right = forward.cross(up).normalized()
    up = right.cross(forward)
    obj.matrix_world = Matrix.Translation(bt.blender(cam["position"])) @ Matrix(
        ((right.x, up.x, -forward.x, 0), (right.y, up.y, -forward.y, 0), (right.z, up.z, -forward.z, 0),
         (0, 0, 0, 1)))
    scene.camera = obj

    # The sky seen where nothing is; without a sky, the view's background (transparent: black).
    scene.render.film_transparent = not doc.get("sky")
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "32"
    os.makedirs(opts["out"], exist_ok=True)
    exr = os.path.join(opts["out"], "cycles.exr")
    scene.render.filepath = exr
    import time
    start = time.time()
    bpy.ops.render.render(write_still=True)
    seconds = time.time() - start

    # The view's exposure and tonemap (kin's color_output: ACES on an HDR view; else clamped), over the background.
    image = bpy.data.images.load(exr)
    px = np.array(image.pixels[:], dtype=np.float32).reshape(height, width, 4)[::-1]
    view = doc["view"]
    rgb, alpha = px[:, :, :3], px[:, :, 3:4]
    if not doc.get("sky"):
        bg = np.array(view["background"][:3], dtype=np.float32)
        rgb = rgb + bg * (1 - np.clip(alpha, 0, 1))
    if view["hdr"]:
        rgb = aces(np.maximum(rgb * view["exposure"], 0.0))
    shown = (srgb(rgb) * 255 + 0.5).astype(np.uint8)
    holes = (alpha[:, :, 0] < 0.5).astype(np.uint8) * 255
    np.save(os.path.join(opts["out"], "holes.npy"), holes)
    out_img = bpy.data.images.new("shown", width, height, alpha=True)
    rgba = np.concatenate([shown, np.full((height, width, 1), 255, np.uint8)], axis=2)[::-1].astype(np.float32) / 255
    out_img.pixels = rgba.ravel()
    out_img.filepath_raw = os.path.join(opts["out"], "cycles.png")
    out_img.file_format = "PNG"
    out_img.save()
    with open(os.path.join(opts["out"], "cycles.json"), "w") as f:
        json.dump({"blender": bpy.app.version_string, "device": device, "samples": int(opts["samples"]),
                   "denoise": opts["denoise"], "seconds": round(seconds, 1), "objects": len(doc["objects"])}, f,
                  indent=1)
    print(f"render_snapshot: {seconds:.1f} s on {device}, wrote {opts['out']}/cycles.png")


if __name__ == "__main__":
    main()
