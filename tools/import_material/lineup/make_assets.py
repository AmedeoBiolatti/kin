"""The lineup's shared assets: textures (PNG) and meshes (glTF .glb), the same files for kin, Godot and Unreal.

    python3 make_assets.py <out folder>     -> checker.png mask.png normal.png quad.glb sphere.glb
"""
import json
import math
import os
import struct
import sys
import zlib

SIZE = 256


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def checker(x, y):
    # 8 x 8 squares; asymmetric gradients so a flipped or mirrored UV shows.
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    if ((x // 32) + (y // 32)) % 2:
        return (230, int(30 + 150 * u), 60, 255)
    return (40, 90, int(60 + 170 * v), 255)


def mask(x, y):
    # Linear data: R a ramp along u, G a disc, B the v ramp.
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    disc = 255 if (u - 0.35) ** 2 + (v - 0.3) ** 2 < 0.04 else 0
    return (int(255 * u + 0.5), disc, int(255 * v + 0.5), 255)


def height(u, v):
    # One dome a tile, off-centre (upper left) so a flipped green channel shows.
    d = math.hypot(u - 0.4, v - 0.35) / 0.3
    return 0.0 if d >= 1 else math.cos(d * math.pi / 2) ** 2


def normal(x, y):
    # OpenGL convention (+y is up in the picture: toward smaller v), as glTF, Godot and kin take them.
    e = 1.0 / SIZE
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    dhdu = (height(u + e, v) - height(u - e, v)) / (2 * e)
    dhdv = (height(u, v + e) - height(u, v - e)) / (2 * e)
    nx, ny, nz = -dhdu * 0.08, dhdv * 0.08, 1.0  # up = -v
    n = math.sqrt(nx * nx + ny * ny + nz * nz)
    return tuple(int((c / n * 0.5 + 0.5) * 255 + 0.5) for c in (nx, ny, nz)) + (255,)


def glb(path, positions, normals, uvs, indices):
    def pack(fmt, rows):
        return b"".join(struct.pack(fmt, *r) for r in rows)
    blobs = [pack("<3f", positions), pack("<3f", normals), pack("<2f", uvs), pack("<I", [(i,) for i in indices])]
    data, views, offset = b"", [], 0
    for b in blobs:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(b)})
        data += b
        offset += len(b)
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    doc = {
        "asset": {"version": "2.0", "generator": "kin material lineup"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}],
        "meshes": [{"name": os.path.splitext(os.path.basename(path))[0],
                    "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
        "bufferViews": views,
        "buffers": [{"byteLength": len(data)}],
    }
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    data += b"\0" * (-len(data) % 4)
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(data), 0x004E4942) + data
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def quad(path, half=0.8):
    p = [(-half, half, 0), (half, half, 0), (half, -half, 0), (-half, -half, 0)]
    uv = [(0, 0), (1, 0), (1, 1), (0, 1)]  # glTF: v down
    glb(path, p, [(0, 0, 1)] * 4, uv, [0, 2, 1, 0, 3, 2])  # counter-clockwise seen from +z


def sphere(path, r=0.8, seg=96, rings=48):
    p, n, uv, idx = [], [], [], []
    for j in range(rings + 1):
        t = math.pi * j / rings
        for i in range(seg + 1):
            u = i / seg
            phi = 2 * math.pi * u
            d = (-math.sin(t) * math.sin(phi), math.cos(t), -math.sin(t) * math.cos(phi))  # u = 0.5 faces +z
            p.append(tuple(r * c for c in d))
            n.append(d)
            uv.append((u, j / rings))
    for j in range(rings):
        for i in range(seg):
            a, b = j * (seg + 1) + i, (j + 1) * (seg + 1) + i
            for tri in ((a, b, a + 1), (a + 1, b, b + 1)):
                # Wound counter-clockwise seen from outside.
                pa, pb, pc = (p[k] for k in tri)
                e1 = [pb[k] - pa[k] for k in range(3)]
                e2 = [pc[k] - pa[k] for k in range(3)]
                c = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                m = [sum(p[k][q] for k in tri) for q in range(3)]
                if sum(c[q] * m[q] for q in range(3)) < 0:
                    tri = (tri[0], tri[2], tri[1])
                if sum(abs(x) for x in c) > 1e-12:
                    idx.extend(tri)
    glb(path, p, n, uv, idx)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    png(os.path.join(out, "checker.png"), SIZE, SIZE, checker)
    png(os.path.join(out, "mask.png"), SIZE, SIZE, mask)
    png(os.path.join(out, "normal.png"), SIZE, SIZE, normal)
    quad(os.path.join(out, "quad.glb"))
    sphere(os.path.join(out, "sphere.glb"))


if __name__ == "__main__":
    main()
