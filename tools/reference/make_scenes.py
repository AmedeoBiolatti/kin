"""The reference scenes made here (docs/3d_plan.md, G0), written as .glb files.

    python3 tools/reference/make_scenes.py <dir>

cornell.glb  the Cornell box, its published geometry (metres) and the usual
             reflectances; the light a panel with emission (Cycles lights the
             box with it; kin's scene file adds a point light under it).
spheres.glb  5 x 5 spheres, metallic by rows (0 to 1, from the front),
             rough by columns (0 to 1, left to right), on a grey floor.
furnace.glb  the same spheres white, without the floor: in an even white
             sky an energy-conserving material disappears.
shadows.glb  poles, thin plates at heights, steps, a slatted fence, a sphere
             and a box resting on the floor: shadow acne, peter-panning,
             aliasing, contact.
skinned_bend.glb  a tube bound to a chain of three joints, with the clips
             "bend" (the middle joint swings, the top counter-swings) and
             "wave" (every joint sways in turn): skinning with no download
             (P1a); skinned_dense.glb the same with 4,096 triangles (the
             200-copy cost scene). floor20.glb is a 20 m floor to stand things on.
skinned_biped.glb  a person of 1.95 m on 18 joints (pelvis, spine, chest, neck, head, a leg and an arm
             each side; the arms out), facing +Z, with the clip "idle" (a sway): what the IK scenes (P1b) pose.
             ik_marker.glb is a small red sphere (a target), ik_step.glb a 0.8 x 0.2 x 0.8 box (a step), ik_ramp.glb a 25 degree wedge.
skinned_biped_walk.glb  the same person (with the toes) and the clips "idle", "walk" (a 1.0 s cycle), "run" (0.6 s) and "fall" (a held
             pose, legs apart, arms out): what the character controller's scene (P2c) blends by ground speed.

Deterministic: the same files every time.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "models"))
from glb import Glb, box, cylinder, polygon, sphere  # noqa: E402
import math  # noqa: E402


def cornell(path):
    mm = 0.001
    p = lambda *c: tuple(v * mm for v in c)  # noqa: E731
    middle = p(278, 274, 280)
    g = Glb()
    white = g.material("white", color=(0.725, 0.71, 0.68, 1))
    red = g.material("red", color=(0.63, 0.065, 0.05, 1))
    green = g.material("green", color=(0.14, 0.45, 0.091, 1))
    light = g.material("light", color=(0.78, 0.78, 0.78, 1), emissive=(1.0, 0.706, 0.235), emissive_strength=17.0)
    walls = [
        ("floor", white, [p(552.8, 0, 0), p(0, 0, 0), p(0, 0, 559.2), p(549.6, 0, 559.2)]),
        ("ceiling", white, [p(556, 548.8, 0), p(556, 548.8, 559.2), p(0, 548.8, 559.2), p(0, 548.8, 0)]),
        ("back", white, [p(549.6, 0, 559.2), p(0, 0, 559.2), p(0, 548.8, 559.2), p(556, 548.8, 559.2)]),
        ("green_wall", green, [p(0, 0, 559.2), p(0, 0, 0), p(0, 548.8, 0), p(0, 548.8, 559.2)]),
        ("red_wall", red, [p(552.8, 0, 0), p(549.6, 0, 559.2), p(556, 548.8, 559.2), p(556, 548.8, 0)]),
    ]
    for name, material, corners in walls:
        g.node(name, mesh=g.mesh(name, *polygon(corners, toward=middle), material=material))
    # The light, a hair below the ceiling, facing down.
    panel = [p(343, 548.3, 227), p(343, 548.3, 332), p(213, 548.3, 332), p(213, 548.3, 227)]
    g.node("light", mesh=g.mesh("light", *polygon(panel, toward=p(278, 0, 280)), material=light))
    blocks = [
        ("short_block", 165, [(130, 65), (82, 225), (240, 272), (290, 114)]),
        ("tall_block", 330, [(423, 247), (265, 296), (314, 456), (472, 406)]),
    ]
    for name, height, top in blocks:
        cx = sum(x for x, _ in top) / 4
        cz = sum(z for _, z in top) / 4
        centre = p(cx, height / 2, cz)
        faces = [[p(x, height, z) for x, z in top]]
        for i in range(4):
            (x0, z0), (x1, z1) = top[i], top[(i + 1) % 4]
            faces.append([p(x0, height, z0), p(x1, height, z1), p(x1, 0, z1), p(x0, 0, z0)])
        for k, face in enumerate(faces):
            # Facing out of the block: toward a point beyond the face's middle.
            mid = tuple(sum(c[i] for c in face) / 4 for i in range(3))
            beyond = tuple(2 * mid[i] - centre[i] for i in range(3))
            positions, normals, uvs, indices = polygon(face, toward=beyond)
            g.node(f"{name}_{k}", mesh=g.mesh(f"{name}_{k}", positions, normals, uvs, indices, material=white))
    g.save(path)


def spheres(path, furnace=False):
    g = Glb()
    ball = sphere(0.4, 48, 24)
    for row in range(5):
        for col in range(5):
            metallic, roughness = row / 4, col / 4
            color = (1, 1, 1, 1) if furnace else (0.9, 0.45, 0.2, 1)
            m = g.material(f"m{row}r{col}", color=color, metallic=metallic, roughness=roughness)
            mesh = g.mesh(f"ball_{row}_{col}", *ball, material=m)
            g.node(f"ball_{row}_{col}", mesh=mesh, translation=(col - 2.0, 0.4, 2.0 - row))
    if not furnace:
        floor = g.material("floor", color=(0.5, 0.5, 0.5, 1))
        g.node("floor", mesh=g.mesh("floor", *box((8, 0.1, 8)), material=floor), translation=(0, -0.05, 0))
    g.save(path)


def shadows(path):
    g = Glb()
    grey = g.material("grey", color=(0.6, 0.6, 0.6, 1))
    light = g.material("light", color=(0.8, 0.75, 0.65, 1))
    dark = g.material("dark", color=(0.25, 0.27, 0.3, 1))
    g.node("floor", mesh=g.mesh("floor", *box((20, 0.2, 20)), material=grey), translation=(0, -0.1, 0))
    # Poles: thin casters, long shadows.
    pole = g.mesh("pole", *cylinder(0.03, 2.0, 12), material=dark)
    for i in range(6):
        g.node(f"pole_{i}", mesh=pole, translation=(-6 + i * 0.5, 1.0, -4), rotation=(-0.7071068, 0, 0, 0.7071068))
    # Thin plates at heights: contact (peter-panning) to well above.
    for i, h in enumerate((0.02, 0.1, 0.4, 1.2)):
        g.node(f"plate_{i}", mesh=g.mesh(f"plate_{i}", *box((1, 0.02, 1)), material=light), translation=(-3 + i * 1.6, h + 0.01, 0))
    # Steps: faces at grazing light.
    for i in range(6):
        g.node(f"step_{i}", mesh=g.mesh(f"step_{i}", *box((2, 0.2 * (i + 1), 0.4)), material=light),
               translation=(4, 0.1 * (i + 1), -2 + i * 0.4))
    # A slatted fence: fine shadow detail.
    for i in range(24):
        g.node(f"slat_{i}", mesh=g.mesh(f"slat_{i}", *box((0.04, 1.0, 0.04)), material=dark), translation=(-5 + i * 0.12, 0.5, 3))
    g.node("rail", mesh=g.mesh("rail", *box((3.0, 0.05, 0.05)), material=dark), translation=(-3.62, 0.95, 3))
    # Resting on the floor: a sphere and a box.
    g.node("ball", mesh=g.mesh("ball", *sphere(0.5, 48, 24), material=light), translation=(2, 0.5, 2.5))
    g.node("crate", mesh=g.mesh("crate", *box((0.8, 0.8, 0.8)), material=light), translation=(0, 0.4, 2.5),
           rotation=(0, 0.3826834, 0, 0.9238795))
    # A long wall the low sun grazes.
    g.node("wall", mesh=g.mesh("wall", *box((8, 1.5, 0.2)), material=light), translation=(0, 0.75, -6))
    g.save(path)


def skinned_bend(path, rings=49, segments=16):
    """A tube along +Y (3 m) bound to three joints at 0, 1 and 2 m, with two clips; written by hand (a skin, inverse
    binds and animations are not in the Glb helper). `rings` x `segments` sets its triangles (the default 1,536)."""
    import json
    import struct
    length, radius = 3.0, 0.3
    third = length / 3.0
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    for r in range(rings):
        y = length * r / (rings - 1)
        for s in range(segments + 1):
            a = 2 * math.pi * s / segments
            n = (math.cos(a), 0.0, -math.sin(a))
            positions.append((n[0] * radius, y, n[2] * radius))
            normals.append(n)
            uvs.append((s / segments, r / (rings - 1)))
            w = [max(0.0, 1.0 - abs(y - third * j) / third) for j in range(3)]
            if y > 2 * third:
                w = [0.0, 0.0, 1.0]
            total = sum(w)
            joints.append((0, 1, 2, 0))
            weights.append((w[0] / total, w[1] / total, w[2] / total, 0.0))
    row = segments + 1
    for r in range(rings - 1):
        for s in range(segments):
            a, b, c, d = r * row + s, r * row + s + 1, (r + 1) * row + s + 1, (r + 1) * row + s
            indices += [a, b, c, a, c, d]
    blob = bytearray()
    views = []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        if target:
            views[-1]["target"] = target
        blob.extend(data)
        return len(views) - 1

    def flat(items):
        return [v for item in items for v in item]
    accessors = []

    def accessor(view, ctype, count, kind, **extra):
        accessors.append({"bufferView": view, "componentType": ctype, "count": count, "type": kind, **extra})
        return len(accessors) - 1
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    a_pos = accessor(add(struct.pack("<%df" % (3 * len(positions)), *flat(positions)), 34962), 5126, len(positions), "VEC3", min=lo, max=hi)
    a_nor = accessor(add(struct.pack("<%df" % (3 * len(normals)), *flat(normals)), 34962), 5126, len(normals), "VEC3")
    a_uv = accessor(add(struct.pack("<%df" % (2 * len(uvs)), *flat(uvs)), 34962), 5126, len(uvs), "VEC2")
    a_joint = accessor(add(struct.pack("<%dB" % (4 * len(joints)), *flat(joints)), 34962), 5121, len(joints), "VEC4")
    a_weight = accessor(add(struct.pack("<%df" % (4 * len(weights)), *flat(weights)), 34962), 5126, len(weights), "VEC4")
    a_index = accessor(add(struct.pack("<%dI" % len(indices), *indices), 34963), 5125, len(indices), "SCALAR")
    ibm = []
    for j in range(3):
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -third * j, 0, 1]
    a_ibm = accessor(add(struct.pack("<48f", *ibm)), 5126, 3, "MAT4")

    def quat(deg):
        return (0.0, 0.0, math.sin(math.radians(deg) / 2), math.cos(math.radians(deg) / 2))
    samplers, channels = [], []

    def track(node, path, times, values, size):
        t = accessor(add(struct.pack("<%df" % len(times), *times)), 5126, len(times), "SCALAR", min=[min(times)], max=[max(times)])
        v = accessor(add(struct.pack("<%df" % (size * len(times)), *flat(values))), 5126, len(times), "VEC4" if size == 4 else "VEC3")
        samplers.append({"input": t, "output": v, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
    # Nodes: 0 base (the root joint), 1 middle, 2 top, 3 the skinned mesh.
    track(1, "rotation", [0.0, 0.5, 1.0, 1.5, 2.0], [quat(0), quat(70), quat(0), quat(-70), quat(0)], 4)
    track(2, "rotation", [0.0, 0.5, 1.0, 1.5, 2.0], [quat(0), quat(-55), quat(0), quat(55), quat(0)], 4)
    bend = {"name": "bend", "samplers": samplers, "channels": channels}
    samplers, channels = [], []
    track(0, "rotation", [0.0, 1.0, 2.0, 3.0, 4.0], [quat(0), quat(15), quat(0), quat(-15), quat(0)], 4)
    track(1, "rotation", [0.0, 1.0, 2.0, 3.0, 4.0], [quat(-20), quat(0), quat(20), quat(0), quat(-20)], 4)
    track(2, "rotation", [0.0, 1.0, 2.0, 3.0, 4.0], [quat(30), quat(0), quat(-30), quat(0), quat(30)], 4)
    wave = {"name": "wave", "samplers": samplers, "channels": channels}
    gltf = {
        "asset": {"version": "2.0", "generator": "kin make_scenes.py"},
        "scene": 0, "scenes": [{"nodes": [0, 3]}],
        "nodes": [{"name": "base", "children": [1]}, {"name": "middle", "translation": [0, third, 0], "children": [2]},
                  {"name": "top", "translation": [0, third, 0]}, {"name": "tube", "mesh": 0, "skin": 0}],
        "meshes": [{"name": "tube", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nor, "TEXCOORD_0": a_uv, "JOINTS_0": a_joint,
                                                                    "WEIGHTS_0": a_weight}, "indices": a_index, "material": 0}]}],
        "materials": [{"name": "clay", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.35, 0.28, 1], "metallicFactor": 0.0, "roughnessFactor": 0.7}}],
        "skins": [{"joints": [0, 1, 2], "inverseBindMatrices": a_ibm, "skeleton": 0}],
        "animations": [bend, wave],
        "buffers": [{"byteLength": len(blob)}], "bufferViews": views, "accessors": accessors,
    }
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    data = bytes(blob) + b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(text) + 8 + len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(data), 0x004E4942) + data)


def skinned_biped(path, extras=False, walk=False):
    """A person on the 18 joints of tests/skin_fixtures.hpp's biped (names and rest placing the same), made of tubes and spheres whose
    weights ramp to the next joint over the last 40% of each bone. Written by hand, as skinned_bend is.
    With `extras` (skinned_biped_tail.glb: the same person, then) two toes (toe_L, toe_R, joints 18 and 19) and a tail of five joints
    (tail_1 to tail_5, 20 to 24, hanging straight back from the pelvis) are added after the 18, for the FABRIK and toe scenes."""
    import json
    import struct
    joint_defs = [  # name, parent, translation
        ("root", -1, (0, 0, 0)), ("pelvis", 0, (0, 1.0, 0)), ("spine", 1, (0, 0.2, 0)), ("chest", 2, (0, 0.25, 0)),
        ("neck", 3, (0, 0.25, 0)), ("head", 4, (0, 0.1, 0)),
        ("hip_L", 1, (0.1, 0, 0)), ("knee_L", 6, (0, -0.5, 0)), ("ankle_L", 7, (0, -0.45, 0)),
        ("hip_R", 1, (-0.1, 0, 0)), ("knee_R", 9, (0, -0.5, 0)), ("ankle_R", 10, (0, -0.45, 0)),
        ("shoulder_L", 3, (0.2, 0.2, 0)), ("elbow_L", 12, (0.5, 0, 0)), ("hand_L", 13, (0.45, 0, 0)),
        ("shoulder_R", 3, (-0.2, 0.2, 0)), ("elbow_R", 15, (-0.5, 0, 0)), ("hand_R", 16, (-0.45, 0, 0)),
    ]
    if extras:
        joint_defs += [("toe_L", 8, (0, -0.03, 0.12)), ("toe_R", 11, (0, -0.03, 0.12)), ("tail_1", 1, (0, -0.05, -0.1)),
                       ("tail_2", 20, (0, 0, -0.18)), ("tail_3", 21, (0, 0, -0.18)), ("tail_4", 22, (0, 0, -0.18)), ("tail_5", 23, (0, 0, -0.18))]
    world = []
    for name, parent, t in joint_defs:
        base = world[parent] if parent >= 0 else (0.0, 0.0, 0.0)
        world.append((base[0] + t[0], base[1] + t[1], base[2] + t[2]))
    index = {name: i for i, (name, _, _) in enumerate(joint_defs)}
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []

    def sub(a, b):
        return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def unit(a):
        n = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)
        return (a[0] / n, a[1] / n, a[2] / n)

    def tube(p0, p1, radius0, radius1, joint_a, joint_b, rings=8, segments=10, ramp=0.6, cap=True):
        d = unit(sub(p1, p0))
        helper = (0.0, 0.0, 1.0) if abs(d[2]) < 0.9 else (1.0, 0.0, 0.0)
        u = unit(cross(helper, d))
        v = cross(d, u)
        first = len(positions)
        for r in range(rings):
            t = r / (rings - 1)
            radius = radius0 + (radius1 - radius0) * t
            w = min(1.0, max(0.0, (t - ramp) / (1.0 - ramp)))
            w = w * w * (3 - 2 * w)
            for sgm in range(segments + 1):
                a = 2 * math.pi * sgm / segments
                n = (u[0] * math.cos(a) + v[0] * math.sin(a), u[1] * math.cos(a) + v[1] * math.sin(a), u[2] * math.cos(a) + v[2] * math.sin(a))
                c = tuple(p0[i] + (p1[i] - p0[i]) * t for i in range(3))
                positions.append(tuple(c[i] + n[i] * radius for i in range(3)))
                normals.append(n)
                uvs.append((sgm / segments, t))
                joints.append((joint_a, joint_b, 0, 0))
                weights.append((1.0 - w, w, 0.0, 0.0))
        row = segments + 1
        for r in range(rings - 1):
            for sgm in range(segments):
                a, b, c, e = first + r * row + sgm, first + r * row + sgm + 1, first + (r + 1) * row + sgm + 1, first + (r + 1) * row + sgm
                # Outward-facing: rings go up the bone, the angle turns from u toward v (u x v = d): a, c, b is counter-clockwise from outside.
                indices.extend([a, c, b, a, e, c])

    def ball(centre, radius, joint, rings=8, segments=12):
        first = len(positions)
        for r in range(rings + 1):
            theta = math.pi * r / rings
            for sgm in range(segments + 1):
                phi = 2 * math.pi * sgm / segments
                n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
                positions.append(tuple(centre[i] + n[i] * radius for i in range(3)))
                normals.append(n)
                uvs.append((sgm / segments, r / rings))
                joints.append((joint, 0, 0, 0))
                weights.append((1.0, 0.0, 0.0, 0.0))
        row = segments + 1
        for r in range(rings):
            for sgm in range(segments):
                a, b, c, e = first + r * row + sgm, first + r * row + sgm + 1, first + (r + 1) * row + sgm + 1, first + (r + 1) * row + sgm
                indices.extend([a, b, c, a, c, e])

    def bone(a, b, r0, r1):
        tube(world[index[a]], world[index[b]], r0, r1, index[a], index[b])

    bone("pelvis", "spine", 0.12, 0.115)
    bone("spine", "chest", 0.115, 0.13)
    bone("chest", "neck", 0.13, 0.05)
    bone("neck", "head", 0.05, 0.045)
    ball((world[5][0], world[5][1] + 0.07, world[5][2]), 0.105, 5)
    ball((world[5][0], world[5][1] + 0.06, world[5][2] + 0.115), 0.032, 5)  # a nose: which way the head faces
    for side in ("L", "R"):
        bone("pelvis", "hip_" + side, 0.08, 0.075)
        bone("hip_" + side, "knee_" + side, 0.075, 0.055)
        bone("knee_" + side, "ankle_" + side, 0.055, 0.04)
        ank = world[index["ankle_" + side]]
        if extras:
            ball_at = world[index["toe_" + side]]
            tube((ank[0], ank[1] - 0.03, ank[2] - 0.05), ball_at, 0.04, 0.038, index["ankle_" + side], index["ankle_" + side], rings=2)
            tube(ball_at, (ank[0], ank[1] - 0.03, ank[2] + 0.14), 0.038, 0.034, index["toe_" + side], index["toe_" + side], rings=2)
        else:
            tube((ank[0], ank[1] - 0.03, ank[2] - 0.05), (ank[0], ank[1] - 0.03, ank[2] + 0.14), 0.04, 0.035, index["ankle_" + side], index["ankle_" + side], rings=2)
        bone("chest", "shoulder_" + side, 0.05, 0.05)
        bone("shoulder_" + side, "elbow_" + side, 0.05, 0.04)
        bone("elbow_" + side, "hand_" + side, 0.04, 0.032)
        ball(world[index["hand_" + side]], 0.055, index["hand_" + side])
    if extras:
        chain = ["pelvis", "tail_1", "tail_2", "tail_3", "tail_4", "tail_5"]
        for k in range(len(chain) - 1):
            tube(world[index[chain[k]]], world[index[chain[k + 1]]], 0.045 - 0.007 * k, 0.045 - 0.007 * (k + 1), index[chain[k]], index[chain[k + 1]],
                 rings=6, ramp=0.5)
    blob = bytearray()
    views = []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        if target:
            views[-1]["target"] = target
        blob.extend(data)
        return len(views) - 1

    def flat(items):
        return [x for item in items for x in item]
    accessors = []

    def accessor(view, ctype, count, kind, **extra):
        accessors.append({"bufferView": view, "componentType": ctype, "count": count, "type": kind, **extra})
        return len(accessors) - 1
    lo = [min(q[i] for q in positions) for i in range(3)]
    hi = [max(q[i] for q in positions) for i in range(3)]
    a_pos = accessor(add(struct.pack("<%df" % (3 * len(positions)), *flat(positions)), 34962), 5126, len(positions), "VEC3", min=lo, max=hi)
    a_nor = accessor(add(struct.pack("<%df" % (3 * len(normals)), *flat(normals)), 34962), 5126, len(normals), "VEC3")
    a_uv = accessor(add(struct.pack("<%df" % (2 * len(uvs)), *flat(uvs)), 34962), 5126, len(uvs), "VEC2")
    a_joint = accessor(add(struct.pack("<%dB" % (4 * len(joints)), *flat(joints)), 34962), 5121, len(joints), "VEC4")
    a_weight = accessor(add(struct.pack("<%df" % (4 * len(weights)), *flat(weights)), 34962), 5126, len(weights), "VEC4")
    a_index = accessor(add(struct.pack("<%dI" % len(indices), *indices), 34963), 5125, len(indices), "SCALAR")
    ibm = []
    for w in world:
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]
    a_ibm = accessor(add(struct.pack("<%df" % len(ibm), *ibm)), 5126, len(world), "MAT4")

    def quat(axis, deg):
        s = math.sin(math.radians(deg) / 2)
        return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(math.radians(deg) / 2))
    samplers, channels = [], []

    def track(node, times, values):
        t = accessor(add(struct.pack("<%df" % len(times), *times)), 5126, len(times), "SCALAR", min=[min(times)], max=[max(times)])
        v = accessor(add(struct.pack("<%df" % (4 * len(times)), *flat(values))), 5126, len(times), "VEC4")
        samplers.append({"input": t, "output": v, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "rotation"}})
    times = [0.0, 1.0, 2.0, 3.0, 4.0]
    track(index["spine"], times, [quat((0, 0, 1), d) for d in (0, 3, 0, -3, 0)])
    track(index["chest"], times, [quat((1, 0, 0), d) for d in (0, 2, 0, -2, 0)])
    track(index["head"], times, [quat((0, 1, 0), d) for d in (0, -4, 0, 4, 0)])
    idle = {"name": "idle", "samplers": samplers, "channels": channels}
    animations = [idle]
    if walk:
        # Locomotion: legs swing about their own X (a negative angle swings the foot forward, +Z), the knee folds the shank
        # back as the leg comes through, the arms swing opposite the legs and the spine bobs a little. Keys eight to a cycle.
        def gait(name, period, hip_deg, knee_deg, arm_deg, lean_deg):
            keys = 8
            times = [period * k / keys for k in range(keys + 1)]
            local_samplers, local_channels = [], []

            def add_track(node, values_deg, axis):
                t = accessor(add(struct.pack("<%df" % len(times), *times)), 5126, len(times), "SCALAR", min=[min(times)], max=[max(times)])
                v = accessor(add(struct.pack("<%df" % (4 * len(times)), *flat([quat(axis, d) for d in values_deg]))), 5126, len(times), "VEC4")
                local_samplers.append({"input": t, "output": v, "interpolation": "LINEAR"})
                local_channels.append({"sampler": len(local_samplers) - 1, "target": {"node": node, "path": "rotation"}})
            phase = [2.0 * math.pi * k / keys for k in range(keys + 1)]
            for side, sign in (("L", 1.0), ("R", -1.0)):
                add_track(index["hip_" + side], [-sign * hip_deg * math.sin(p) for p in phase], (1, 0, 0))
                # the knee folds while the leg swings forward through the middle of the cycle
                add_track(index["knee_" + side], [knee_deg * max(0.0, math.cos(p + (0.0 if sign > 0 else math.pi))) for p in phase], (1, 0, 0))
                add_track(index["shoulder_" + side], [sign * arm_deg * math.sin(p) for p in phase], (0, 1, 0))
            add_track(index["spine"], [lean_deg + 1.5 * math.sin(2.0 * p) for p in phase], (1, 0, 0))
            return {"name": name, "samplers": local_samplers, "channels": local_channels}
        animations.append(gait("walk", 1.0, 26.0, 38.0, 18.0, 2.0))
        animations.append(gait("run", 0.6, 42.0, 70.0, 38.0, 9.0))
        # fall: one held pose (two keys): legs a little apart, knees bent, arms swung up and out.
        def held(name):
            local_samplers, local_channels = [], []
            times = [0.0, 1.0]

            def add_track(node, deg, axis):
                t = accessor(add(struct.pack("<%df" % len(times), *times)), 5126, len(times), "SCALAR", min=[0.0], max=[1.0])
                v = accessor(add(struct.pack("<%df" % (4 * len(times)), *flat([quat(axis, deg)] * 2))), 5126, len(times), "VEC4")
                local_samplers.append({"input": t, "output": v, "interpolation": "LINEAR"})
                local_channels.append({"sampler": len(local_samplers) - 1, "target": {"node": node, "path": "rotation"}})
            add_track(index["hip_L"], -20.0, (1, 0, 0))
            add_track(index["hip_R"], 12.0, (1, 0, 0))
            add_track(index["knee_L"], 30.0, (1, 0, 0))
            add_track(index["knee_R"], 55.0, (1, 0, 0))
            add_track(index["shoulder_L"], -35.0, (0, 0, 1))
            add_track(index["shoulder_R"], 35.0, (0, 0, 1))
            return {"name": name, "samplers": local_samplers, "channels": local_channels}
        animations.append(held("fall"))
    nodes = []
    for i, (name, parent, t) in enumerate(joint_defs):
        node = {"name": name, "translation": list(t)}
        kids = [j for j, (_, pj, _) in enumerate(joint_defs) if pj == i]
        if kids:
            node["children"] = kids
        nodes.append(node)
    nodes.append({"name": "person", "mesh": 0, "skin": 0})
    gltf = {
        "asset": {"version": "2.0", "generator": "kin make_scenes.py"},
        "scene": 0, "scenes": [{"nodes": [0, len(nodes) - 1]}],
        "nodes": nodes,
        "meshes": [{"name": "person", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nor, "TEXCOORD_0": a_uv, "JOINTS_0": a_joint,
                                                                    "WEIGHTS_0": a_weight}, "indices": a_index, "material": 0}]}],
        "materials": [{"name": "clay", "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.5, 0.36, 1], "metallicFactor": 0.0, "roughnessFactor": 0.65}}],
        "skins": [{"joints": list(range(len(joint_defs))), "inverseBindMatrices": a_ibm, "skeleton": 0}],
        "animations": animations,
        "buffers": [{"byteLength": len(blob)}], "bufferViews": views, "accessors": accessors,
    }
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    data = bytes(blob) + b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(text) + 8 + len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(data), 0x004E4942) + data)


def ik_props(marker_path, step_path):
    """A small red sphere (an IK target, unlit-looking by its emission) and a step: a 0.8 x 0.2 x 0.8 box standing on y = 0."""
    g = Glb()
    red = g.material("target", color=(0.9, 0.1, 0.08, 1), emissive=(1.0, 0.1, 0.05), emissive_strength=2.0)
    g.node("marker", mesh=g.mesh("marker", *sphere(0.04, 16, 8), material=red))
    g.save(marker_path)
    h = Glb()
    stone = h.material("stone", color=(0.45, 0.46, 0.5, 1))
    positions, normals, uvs, indices = box((0.8, 0.2, 0.8))
    h.node("step", mesh=h.mesh("step", [(x, y + 0.1, z) for x, y, z in positions], normals, uvs, indices, material=stone))
    h.save(step_path)


def ik_ramp(path, degrees=25.0, length=0.8, width=1.2):
    """A wedge standing on y = 0, rising along +z from its z = 0 edge at `degrees` (skinned_ik_limits' ramp under a person's toes)."""
    import math as _m
    g = Glb()
    stone = g.material("ramp", color=(0.45, 0.46, 0.5, 1))
    h = length * _m.tan(_m.radians(degrees))
    w = width / 2
    A, B, C, D = (-w, 0, 0), (w, 0, 0), (w, h, length), (-w, h, length)
    Eb, Fb = (-w, 0, length), (w, 0, length)
    n_top = (0.0, _m.cos(_m.radians(degrees)), -_m.sin(_m.radians(degrees)))
    faces = [  # (normal, [triangles of points])
        (n_top, [(A, D, C), (A, C, B)]),
        ((0, 0, 1), [(Eb, Fb, C), (Eb, C, D)]),
        ((0, -1, 0), [(A, B, Fb), (A, Fb, Eb)]),
        ((1, 0, 0), [(B, C, Fb)]),
        ((-1, 0, 0), [(A, Eb, D)]),
    ]
    positions, normals, uvs, indices = [], [], [], []
    for n, tris in faces:
        for tri in tris:
            for pnt in tri:
                indices.append(len(positions))
                positions.append(pnt)
                normals.append(n)
                uvs.append((pnt[0], pnt[2]))
    g.node("ramp", mesh=g.mesh("ramp", positions, normals, uvs, indices, material=stone))
    g.save(path)


def floor20(path):
    """A 20 m square floor, its top at y = 0, light grey."""
    g = Glb()
    grey = g.material("floor", color=(0.62, 0.62, 0.6, 1))
    positions, normals, uvs, indices = box((20.0, 0.2, 20.0))
    g.node("floor", mesh=g.mesh("floor", [(x, y - 0.1, z) for x, y, z in positions], normals, uvs, indices, material=grey))
    g.save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    cornell(os.path.join(out, "cornell.glb"))
    spheres(os.path.join(out, "spheres.glb"))
    spheres(os.path.join(out, "furnace.glb"), furnace=True)
    shadows(os.path.join(out, "shadows.glb"))
    skinned_bend(os.path.join(out, "skinned_bend.glb"))
    skinned_bend(os.path.join(out, "skinned_dense.glb"), rings=65, segments=32)  # 4,096 triangles
    floor20(os.path.join(out, "floor20.glb"))
    skinned_biped(os.path.join(out, "skinned_biped.glb"))
    skinned_biped(os.path.join(out, "skinned_biped_tail.glb"), extras=True)
    skinned_biped(os.path.join(out, "skinned_biped_walk.glb"), walk=True)
    ik_props(os.path.join(out, "ik_marker.glb"), os.path.join(out, "ik_step.glb"))
    ik_ramp(os.path.join(out, "ik_ramp.glb"))


if __name__ == "__main__":
    main()
