"""The ground truth of a reference scene (docs/3d_plan.md, G0): Blender's
Cycles rendering the same models from the same cameras, in the same light.

    blender -b --factory-startup --python tools/reference/blender_truth.py -- \\
        <scene.json> --kin <kin_reference's results.json> --cache <dir> --out <dir> [--samples N] [--seed N] [--denoise]
    blender -b --factory-startup --python tools/reference/blender_truth.py -- --calibrate

The cameras are read from kin's results (where kin_reference placed them:
the same eye, target and field of view). kin's lights become Blender's:

- the sun (`color` c, linear) a Sun lamp of strength pi * max(c): a diffuse
  surface facing it then shows albedo * c, as kin's does (Cycles' diffuse
  is albedo * irradiance / pi); `--calibrate` checks it on a white plane;
- the fill a second Sun lamp that casts no shadow;
- the environment (an equirectangular .hdr) the world, the same picture,
  intensity and rotation;
- the ambient the sky's colour (with `ground_ambient`, the sky's below):
  kin adds it unshadowed, Cycles lets the scene hide it;
- point lights are not mapped (kin's fade to nothing at their radius;
  Cycles' fall off with the square of distance): a scene names the truth's
  own lights, if any, under `truth.lights`; emitting materials light the
  scene in Cycles by themselves.

`truth`: `world` (a colour: the sky instead of the ambient), `sun` (false:
not mapped), `samples`, `lights` (extra lamps: point, sun or area, in kin's
coordinates). The picture is rendered with a transparent background and laid
over the view's background colour, as kin's is; the Standard view
transform, no look, so that the pictures compare as stored.
"""
import json
import math
import os
import sys
import time

import bpy
from mathutils import Vector


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opts = {"samples": None, "calibrate": False, "seed": 0, "denoise": False}
    rest = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--kin", "--cache", "--out", "--samples", "--seed"):
            opts[a[2:]] = argv[i + 1]
            i += 2
        elif a == "--denoise":
            opts["denoise"] = True
            i += 1
        elif a == "--calibrate":
            opts["calibrate"] = True
            i += 1
        else:
            rest.append(a)
            i += 1
    opts["scene"] = rest[0] if rest else None
    return opts


def blender(v):
    """kin's y up to Blender's z up: (x, y, z) -> (x, -z, y)."""
    return Vector((v[0], -v[2], v[1]))


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def setup_render(width, height, samples, seed=0, denoise=False):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    device = "CPU"
    for kind in ("OPTIX", "CUDA", "HIP", "ONEAPI", "METAL"):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            usable = [d for d in prefs.devices if d.type == kind]
            if usable:
                for d in prefs.devices:
                    d.use = d.type == kind
                device = "GPU"
                break
        except TypeError:
            continue
    scene.cycles.device = device
    scene.cycles.samples = samples
    scene.cycles.seed = int(seed)
    scene.cycles.use_denoising = denoise
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.sample_clamp_direct = 0
    scene.cycles.sample_clamp_indirect = 0
    scene.cycles.max_bounces = 16
    scene.cycles.diffuse_bounces = 8
    scene.cycles.glossy_bounces = 8
    scene.cycles.transmission_bounces = 12
    scene.cycles.transparent_max_bounces = 64
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    return device


def world(above, below=None):
    w = bpy.data.worlds.new("sky")
    bpy.context.scene.world = w
    w.use_nodes = True
    nodes, links = w.node_tree.nodes, w.node_tree.links
    background = nodes["Background"]
    background.inputs["Strength"].default_value = 1.0
    if below is None:
        background.inputs["Color"].default_value = (*above, 1.0)
        return
    # Above to below by the direction's height, as kin mixes its ambients by a normal's.
    coord = nodes.new("ShaderNodeTexCoord")
    split = nodes.new("ShaderNodeSeparateXYZ")
    to01 = nodes.new("ShaderNodeMapRange")
    to01.inputs["From Min"].default_value = -1.0
    to01.inputs["From Max"].default_value = 1.0
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.inputs["A"].default_value = (*below, 1.0)
    mix.inputs["B"].default_value = (*above, 1.0)
    links.new(coord.outputs["Generated"], split.inputs[0])
    links.new(split.outputs["Z"], to01.inputs["Value"])
    links.new(to01.outputs["Result"], mix.inputs["Factor"])
    links.new(mix.outputs["Result"], background.inputs["Color"])


def environment(path, intensity=1.0, rotation=0.0):
    """kin's environment (render3d/environment3d.hpp) as the world: the same
    equirectangular .hdr, Blender's own mapping of it (the centre is +x in
    kin's axes too, the top +y, going right turns toward +z: the same as
    kin's, checked by a mirror sphere), `intensity` times, turned about +y by
    `rotation` degrees (the lookup is turned back by as much)."""
    w = bpy.data.worlds.new("environment")
    bpy.context.scene.world = w
    w.use_nodes = True
    nodes, links = w.node_tree.nodes, w.node_tree.links
    background = nodes["Background"]
    background.inputs["Strength"].default_value = intensity
    coord = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeMapping")
    mapping.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(-rotation))
    texture = nodes.new("ShaderNodeTexEnvironment")
    texture.image = bpy.data.images.load(path)
    texture.interpolation = "Linear"
    links.new(coord.outputs["Generated"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
    links.new(texture.outputs["Color"], background.inputs["Color"])


def sun(name, toward, color, shadow=True):
    peak = max(color) if max(color) > 0 else 1.0
    data = bpy.data.lights.new(name, "SUN")
    data.energy = math.pi * peak
    data.color = tuple(c / peak for c in color)
    data.angle = 0.0
    data.use_shadow = shadow
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    # A sun shines along its -Z: point -Z away from the sun.
    obj.rotation_euler = (-blender(toward)).to_track_quat("-Z", "Y").to_euler()
    return obj


def camera(eye, target, fov, width, height):
    data = bpy.data.cameras.new("camera")
    data.sensor_fit = "VERTICAL"
    data.angle = math.radians(fov)
    data.clip_start = 0.005
    data.clip_end = 10000.0
    obj = bpy.data.objects.new("camera", data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = blender(eye)
    obj.rotation_euler = (blender(target) - blender(eye)).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = obj
    return obj


def calibrate():
    """A white diffuse plane facing a sun of kin colour 0.5: kin shows 0.5 linear (sRGB 188)."""
    clear()
    setup_render(64, 64, 64)
    bpy.ops.mesh.primitive_plane_add(size=10)
    plane = bpy.context.object
    mat = bpy.data.materials.new("white")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (1, 1, 1, 1)
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Specular IOR Level"].default_value = 0.0
    plane.data.materials.append(mat)
    world((0, 0, 0))
    sun("sun", (0, 1, 0), (0.5, 0.5, 0.5))
    camera((0, 3, 0.001), (0, 0, 0), 30, 64, 64)
    bpy.context.scene.render.film_transparent = False
    path = os.path.abspath("calibrate.png")
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    pixels = bpy.data.images.load(path).pixels
    print(f"calibration: the plane shows {pixels[(32 * 64 + 32) * 4]:.4f} (stored sRGB 0..1); kin shows 0.7354 (linear 0.5)")


def main():
    opts = args()
    if opts["calibrate"]:
        calibrate()
        return
    doc = json.load(open(opts["scene"]))
    name = doc.get("name", os.path.splitext(os.path.basename(opts["scene"]))[0])
    truth = doc.get("truth")
    if truth is None:
        print(f"{name}: no truth (a speed scene)")
        return
    kin = json.load(open(opts["kin"]))
    width, height = kin["size"]
    cams = next((s["cameras"] for s in kin["scenes"] if s["name"] == name), None)
    if not cams:
        sys.exit(f"{name}: not in {opts['kin']} (run kin_reference first)")
    samples = int(opts["samples"] or truth.get("samples", 512))

    clear()
    device = setup_render(width, height, samples, opts["seed"], opts["denoise"])
    for m in doc.get("models", []):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(opts["cache"], m["file"]))
        added = [o for o in bpy.data.objects if o not in before]
        holder = bpy.data.objects.new("placed", None)
        bpy.context.scene.collection.objects.link(holder)
        for o in added:
            if o.parent is None:
                o.parent = holder
        holder.location = blender(m.get("position", (0, 0, 0)))
        holder.rotation_euler = (0, 0, math.radians(m.get("rotation_y", 0.0)))
        holder.scale = (m.get("scale", 1.0),) * 3

    lights = doc.get("lights", {})
    if "environment" in lights:
        env = lights["environment"]
        environment(os.path.join(opts["cache"], env["file"]), env.get("intensity", 1.0), env.get("rotation", 0.0))
    elif "world" in truth:
        world(tuple(truth["world"]))
    elif "ground_ambient" in lights:
        world(tuple(lights.get("ambient", (0, 0, 0))), tuple(lights["ground_ambient"]))
    else:
        world(tuple(lights.get("ambient", (0, 0, 0))))
    if truth.get("sun", True) and "sun" in lights and max(lights["sun"]["color"]) > 0:
        sun("sun", lights["sun"]["direction"], lights["sun"]["color"])
    if truth.get("sun", True) and "fill" in lights and max(lights["fill"]["color"]) > 0:
        sun("fill", lights["fill"]["direction"], lights["fill"]["color"], shadow=False)
    for i, l in enumerate(truth.get("lights", [])):
        if l["type"] == "sun":
            sun(f"truth_sun_{i}", l["direction"], l["color"])
            continue
        data = bpy.data.lights.new(f"truth_{i}", l["type"].upper())
        data.energy = l.get("power", 100.0)
        data.color = tuple(l.get("color", (1, 1, 1)))
        if l["type"] == "area":
            data.shape = "RECTANGLE"
            data.size, data.size_y = l.get("size", (1, 1))
        obj = bpy.data.objects.new(f"truth_{i}", data)
        bpy.context.scene.collection.objects.link(obj)
        obj.location = blender(l["position"])
        if "toward" in l:
            obj.rotation_euler = (blender(l["toward"]) - blender(l["position"])).to_track_quat("-Z", "Y").to_euler()

    background = doc.get("view", {}).get("background", (0, 0, 0))
    out = os.path.join(opts["out"], name)
    os.makedirs(out, exist_ok=True)
    record = {"profile": "kin.reference.cycles/2", "blender": bpy.app.version_string, "device": device, "samples": samples, "seed": int(opts["seed"]), "denoise": opts["denoise"], "adaptive_sampling": False, "clamp_direct": 0, "clamp_indirect": 0, "cameras": {}}
    for c in cams:
        camera(c["eye"], c["target"], c["fov"], width, height)
        path = os.path.join(out, c["name"] + ".png")
        bpy.context.scene.render.filepath = path
        start = time.time()
        bpy.ops.render.render(write_still=True)
        seconds = time.time() - start
        # Over the view's background, as kin draws it.
        image = bpy.data.images.load(path)
        px = list(image.pixels)
        bg = [((b / 255.0)) for b in background[:3]]
        for i in range(0, len(px), 4):
            a = px[i + 3]
            for k in range(3):
                px[i + k] = px[i + k] * a + bg[k] * (1 - a)
            px[i + 3] = 1.0
        image.pixels = px
        image.filepath_raw = path
        image.file_format = "PNG"
        image.save()
        bpy.data.images.remove(image)
        record["cameras"][c["name"]] = {"seconds": round(seconds, 2)}
        print(f"{name}/{c['name']}: {seconds:.1f} s")
    with open(os.path.join(out, "truth.json"), "w") as f:
        json.dump(record, f, indent=1)


main()
