#!/usr/bin/env python3
"""Writes the atrium's simple models (.glb) and its water's ripple map (.png) into content/.

The shapes are plain on purpose: the atrium's material graphs (content/materials/*.kingraph) give them their look,
reading the world position, so a box scaled to a wall or a floor keeps its pattern's size. Run it again after changing
it; the files it writes are kept in the repository.
"""
import json
import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "content"


def glb(path, positions, normals, uvs, indices, name):
    """One mesh, one material named `name`, as a binary glTF."""
    def pack(fmt, values):
        return b"".join(struct.pack(fmt, *v) if isinstance(v, tuple) else struct.pack(fmt, v) for v in values)

    blobs = [pack("<3f", positions), pack("<3f", normals), pack("<2f", uvs), pack("<I", indices)]
    views, offset, data = [], 0, b""
    for blob, target in zip(blobs, [34962, 34962, 34962, 34963]):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob), "target": target})
        data += blob
        offset += len(blob)
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    doc = {
        "asset": {"version": "2.0", "generator": "kin planar_views make_content.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.8, 0.8, 1], "metallicFactor": 0, "roughnessFactor": 0.6}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
        "bufferViews": views,
        "buffers": [{"byteLength": len(data)}],
    }
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    data += b"\0" * (-len(data) % 4)
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(data), 0x004E4942) + data
    path.write_bytes(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def box():
    """A unit cube, flat faces."""
    p, n, t, i = [], [], [], []
    for axis in range(3):
        for sign in (1, -1):
            normal = [0, 0, 0]
            normal[axis] = sign
            u_axis, v_axis = [(1, 2), (2, 0), (0, 1)][axis]
            base = len(p)
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                c = [0.0, 0.0, 0.0]
                c[axis] = 0.5 * sign
                c[u_axis] = 0.5 * a
                c[v_axis] = 0.5 * b * sign
                p.append(tuple(c))
                n.append(tuple(float(x) for x in normal))
                t.append(((a + 1) / 2, (b + 1) / 2))
            i += [base, base + 1, base + 2, base, base + 2, base + 3]
    return p, n, t, i


def cylinder(segments=40):
    """Radius 0.5, from y 0 to 1, with caps: a column's shaft."""
    p, n, t, i = [], [], [], []
    for k in range(segments + 1):
        a = 2 * math.pi * k / segments
        x, z = math.cos(a), math.sin(a)
        for y in (0.0, 1.0):
            p.append((0.5 * x, y, 0.5 * z))
            n.append((x, 0.0, z))
            t.append((k / segments, y))
    for k in range(segments):
        a, b = 2 * k, 2 * k + 2
        i += [a, a + 1, b + 1, a, b + 1, b]
    for y, ny in ((0.0, -1.0), (1.0, 1.0)):
        centre = len(p)
        p.append((0.0, y, 0.0))
        n.append((0.0, ny, 0.0))
        t.append((0.5, 0.5))
        for k in range(segments + 1):
            a = 2 * math.pi * k / segments
            p.append((0.5 * math.cos(a), y, 0.5 * math.sin(a)))
            n.append((0.0, ny, 0.0))
            t.append((0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)))
        for k in range(segments):
            a, b = centre + 1 + k, centre + 2 + k
            i += [centre, b, a] if ny > 0 else [centre, a, b]
    return p, n, t, i


def sphere(segments=48, rings=24):
    """Radius 0.5."""
    p, n, t, i = [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        phi = math.pi * v
        for s in range(segments + 1):
            u = s / segments
            theta = 2 * math.pi * u
            d = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            p.append(tuple(0.5 * c for c in d))
            n.append(d)
            t.append((u, v))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            i += [a, a + 1, b + 1, a, b + 1, b]
    return p, n, t, i


def torus(radius=0.6, tube=0.18, segments=64, sides=24):
    p, n, t, i = [], [], [], []
    for s in range(segments + 1):
        u = 2 * math.pi * s / segments
        for k in range(sides + 1):
            v = 2 * math.pi * k / sides
            normal = (math.cos(v) * math.cos(u), math.sin(v), math.cos(v) * math.sin(u))
            p.append(((radius + tube * math.cos(v)) * math.cos(u), tube * math.sin(v),
                      (radius + tube * math.cos(v)) * math.sin(u)))
            n.append(normal)
            t.append((s / segments, k / sides))
    for s in range(segments):
        for k in range(sides):
            a = s * (sides + 1) + k
            b = a + sides + 1
            i += [a, b + 1, b, a, a + 1, b + 1]
    return p, n, t, i


def png(path, width, height, pixel):
    rows = b"".join(b"\0" + bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def ripples(n=256):
    """Water's ripples as a tiling normal map (linear, green up): waves of whole periods across the square."""
    waves = [(2, 1, 1.0, 0.0), (-1, 3, 0.7, 1.3), (4, -3, 0.35, 2.1), (3, 7, 0.22, 0.4), (-9, -4, 0.12, 3.0),
             (11, 6, 0.08, 5.2), (-6, 13, 0.06, 1.7)]
    def pixel(x, y):
        dx = dy = 0.0
        for cx, cy, h, phase in waves:
            c = math.cos(2 * math.pi * (cx * x + cy * y) / n + phase) * h * 0.06
            dx += c * cx
            dy += c * cy
        length = math.sqrt(dx * dx + dy * dy + 1)
        return tuple(round((v / length * 0.5 + 0.5) * 255) for v in (-dx, -dy, 1))
    return pixel


def main():
    (ROOT / "models").mkdir(parents=True, exist_ok=True)
    (ROOT / "textures").mkdir(parents=True, exist_ok=True)
    for name, shape in (("box", box()), ("column", cylinder()), ("sphere", sphere()), ("torus", torus())):
        glb(ROOT / "models" / f"{name}.glb", *shape, name)
    png(ROOT / "textures" / "ripples.png", 256, 256, ripples())


if __name__ == "__main__":
    main()
