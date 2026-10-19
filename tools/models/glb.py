"""Writes small glTF 2.0 binary files (.glb) from Python, standard library only.

kin's tests and examples make their models with this (kin/render3d/model3d.hpp
reads them): boxes, quads, prisms, spheres and polygons with materials
(metallic-roughness factors, emissive strength) and embedded PNG textures.

    model = Glb()
    red = model.material(color=(1, 0, 0, 1))
    box = model.mesh("box", *box((1, 1, 1)), material=red)
    model.node("base", mesh=box, children=[...])
    model.save("out.glb")
"""
import json
import math
import struct
import zlib


def png(width, height, rgba):
    """A PNG file of `rgba` (rows of width * 4 bytes, top first)."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def box(size, color=None):
    """A box's positions, normals, UVs and indices, centred, flat faces."""
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    faces = [((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
             ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
             ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
             ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
             ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
             ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)])]
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in faces:
        base = len(positions)
        positions += corners
        normals += [normal] * 4
        uvs += [(0, 1), (1, 1), (1, 0), (0, 0)]
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def sphere(radius, segments=32, rings=16):
    """A UV sphere, smooth, its poles on Y."""
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = math.pi * v
        for s in range(segments + 1):
            u = s / segments
            theta = 2 * math.pi * u
            n = (math.sin(phi) * math.cos(theta), math.cos(phi), -math.sin(phi) * math.sin(theta))
            positions.append(tuple(c * radius for c in n))
            normals.append(n)
            uvs.append((u, v))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            if r != 0:
                indices += [a, b, a + 1]
            if r != rings - 1:
                indices += [a + 1, b, b + 1]
    return positions, normals, uvs, indices


def polygon(corners, toward=None):
    """A flat convex polygon (a fan), its normal from its corners' order, or
    turned to face `toward` (a point) when given."""
    a, b, c = corners[0], corners[1], corners[2]
    u = [b[i] - a[i] for i in range(3)]
    w = [c[i] - a[i] for i in range(3)]
    n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
    if toward is not None and sum(n[i] * (toward[i] - a[i]) for i in range(3)) < 0:
        corners = list(reversed(corners))
        n = [-x for x in n]
    length = math.sqrt(sum(x * x for x in n)) or 1.0
    n = tuple(x / length for x in n)
    indices = []
    for i in range(1, len(corners) - 1):
        indices += [0, i, i + 1]
    return list(corners), [n] * len(corners), [(0, 0)] * len(corners), indices


def quad(width, height):
    """A quad in the XY plane facing +Z, UV (0, 0) at its top left; no normals."""
    w, h = width / 2, height / 2
    return [(-w, -h, 0), (w, -h, 0), (w, h, 0), (-w, h, 0)], None, [(0, 1), (1, 1), (1, 0), (0, 0)], [0, 1, 2, 0, 2, 3]


def cylinder(radius, length, segments=16):
    """A closed cylinder along Z (-length/2 to +length/2), smooth sides, flat caps."""
    positions, normals, uvs, indices = [], [], [], []
    h = length / 2
    for i in range(segments + 1):
        a = 2 * math.pi * i / segments
        x, y = math.cos(a), math.sin(a)
        positions += [(x * radius, y * radius, h), (x * radius, y * radius, -h)]
        normals += [(x, y, 0), (x, y, 0)]
        uvs += [(i / segments, 0), (i / segments, 1)]
    for i in range(segments):
        a, b, c, d = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
        indices += [a, b, d, a, d, c]
    for z, n in ((h, 1), (-h, -1)):
        centre = len(positions)
        positions.append((0, 0, z))
        normals.append((0, 0, n))
        uvs.append((0.5, 0.5))
        for i in range(segments):
            a = 2 * math.pi * i / segments
            positions.append((math.cos(a) * radius, math.sin(a) * radius, z))
            normals.append((0, 0, n))
            uvs.append((0.5 + math.cos(a) / 2, 0.5 + math.sin(a) / 2))
        for i in range(segments):
            p, q = centre + 1 + i, centre + 1 + (i + 1) % segments
            indices += [centre, p, q] if n > 0 else [centre, q, p]
    return positions, normals, uvs, indices


class Glb:
    def __init__(self):
        self.json = {"asset": {"version": "2.0", "generator": "kin tools/models/glb.py"}, "scenes": [{"nodes": []}],
                     "scene": 0, "nodes": [], "meshes": [], "materials": [], "accessors": [], "bufferViews": [],
                     "buffers": [], "images": [], "textures": [], "samplers": []}
        self.bin = bytearray()
        self.extensions = set()

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin += b"\x00"
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.json["bufferViews"].append(view)
        return len(self.json["bufferViews"]) - 1

    def _accessor(self, values, kind, component_type=5126, target=34962):
        flat = [c for v in values for c in (v if isinstance(v, tuple) else (v,))]
        fmt = {5126: "f", 5125: "I", 5123: "H"}[component_type]
        view = self._view(struct.pack("<%d%s" % (len(flat), fmt), *flat), target)
        accessor = {"bufferView": view, "componentType": component_type, "count": len(values), "type": kind}
        if kind == "VEC3" and component_type == 5126:
            accessor["min"] = [min(v[i] for v in values) for i in range(3)]
            accessor["max"] = [max(v[i] for v in values) for i in range(3)]
        self.json["accessors"].append(accessor)
        return len(self.json["accessors"]) - 1

    def texture(self, width, height, rgba, nearest=False):
        image = self._view(png(width, height, rgba))
        self.json["images"].append({"bufferView": image, "mimeType": "image/png"})
        sampler = {"magFilter": 9728 if nearest else 9729, "minFilter": 9728 if nearest else 9987}
        self.json["samplers"].append(sampler)
        self.json["textures"].append({"source": len(self.json["images"]) - 1, "sampler": len(self.json["samplers"]) - 1})
        return len(self.json["textures"]) - 1

    def material(self, name="", color=(1, 1, 1, 1), texture=None, emissive=(0, 0, 0), blend=False, double_sided=False,
                 unlit=False, metallic=0.0, roughness=1.0, emissive_strength=None, metallic_roughness_texture=None,
                 normal_texture=None, normal_scale=1.0, occlusion_texture=None, occlusion_strength=1.0,
                 emissive_texture=None, alpha_cutoff=None):
        pbr = {"baseColorFactor": list(color), "metallicFactor": metallic, "roughnessFactor": roughness}
        if texture is not None:
            pbr["baseColorTexture"] = {"index": texture}
        if metallic_roughness_texture is not None:
            pbr["metallicRoughnessTexture"] = {"index": metallic_roughness_texture}
        m = {"name": name, "pbrMetallicRoughness": pbr, "emissiveFactor": list(emissive), "doubleSided": double_sided}
        if normal_texture is not None:
            m["normalTexture"] = {"index": normal_texture, "scale": normal_scale}
        if occlusion_texture is not None:
            m["occlusionTexture"] = {"index": occlusion_texture, "strength": occlusion_strength}
        if emissive_texture is not None:
            m["emissiveTexture"] = {"index": emissive_texture}
        if blend:
            m["alphaMode"] = "BLEND"
        elif alpha_cutoff is not None:
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = alpha_cutoff
        if unlit:
            m.setdefault("extensions", {})["KHR_materials_unlit"] = {}
            self.extensions.add("KHR_materials_unlit")
        if emissive_strength is not None:
            m.setdefault("extensions", {})["KHR_materials_emissive_strength"] = {"emissiveStrength": emissive_strength}
            self.extensions.add("KHR_materials_emissive_strength")
        self.json["materials"].append(m)
        return len(self.json["materials"]) - 1

    def mesh(self, name, positions, normals, uvs, indices, material=None):
        attributes = {"POSITION": self._accessor(positions, "VEC3")}
        if normals:
            attributes["NORMAL"] = self._accessor(normals, "VEC3")
        if uvs:
            attributes["TEXCOORD_0"] = self._accessor(uvs, "VEC2")
        primitive = {"attributes": attributes, "indices": self._accessor(indices, "SCALAR", 5125, 34963)}
        if material is not None:
            primitive["material"] = material
        self.json["meshes"].append({"name": name, "primitives": [primitive]})
        return len(self.json["meshes"]) - 1

    def node(self, name, mesh=None, translation=None, rotation=None, scale=None, children=None, root=True):
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if translation:
            n["translation"] = list(translation)
        if rotation:
            n["rotation"] = list(rotation)
        if scale:
            n["scale"] = list(scale)
        if children:
            n["children"] = children
        self.json["nodes"].append(n)
        index = len(self.json["nodes"]) - 1
        if root:
            self.json["scenes"][0]["nodes"].append(index)
        return index

    def save(self, path):
        doc = dict(self.json)
        for key in ("images", "textures", "samplers", "materials"):
            if not doc[key]:
                del doc[key]
        if self.extensions:
            doc["extensionsUsed"] = sorted(self.extensions)
        while len(self.bin) % 4:
            self.bin += b"\x00"
        doc["buffers"] = [{"byteLength": len(self.bin)}]
        text = json.dumps(doc, separators=(",", ":")).encode()
        while len(text) % 4:
            text += b" "
        body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)
