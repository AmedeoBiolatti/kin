"""A kin-render-studio mesh export (its mesh.json and the .bin it names) as
a .glb: the `kiln` reference scene (docs/3d_plan.md, G0), local only.

    python3 tools/reference/studio_mesh_to_glb.py <mesh.json> <out.glb>

The export is Blender's: z up, a triangle each 9 positions, 3 normals, a
material and a part; colours sRGB. The .glb is glTF's: y up (x, z, -y), a
primitive a material, flat normals, linear colours; emitting materials
emit their colour. Needs numpy.
"""
import json
import struct
import sys

import numpy as np


def linear(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def main():
    record, out = sys.argv[1], sys.argv[2]
    meta = json.load(open(record))
    meta = meta.get("result", meta)
    n = int(meta["triangles"])
    raw = open(meta["path"], "rb").read()
    at = 0
    positions = np.frombuffer(raw, np.float32, n * 9, at).reshape(n, 3, 3)
    at += n * 9 * 4
    normals = np.frombuffer(raw, np.float32, n * 3, at).reshape(n, 3)
    at += n * 3 * 4
    material = np.frombuffer(raw, np.uint16, n, at)
    # z up to y up: (x, y, z) -> (x, z, -y)
    positions = np.stack([positions[..., 0], positions[..., 2], -positions[..., 1]], axis=-1)
    normals = np.stack([normals[:, 0], normals[:, 2], -normals[:, 1]], axis=-1)
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.where(lengths > 1e-12, normals / np.maximum(lengths, 1e-12), np.array([0.0, 1.0, 0.0], np.float32))
    # Each triangle wound to face its normal.
    across = np.cross(positions[:, 1] - positions[:, 0], positions[:, 2] - positions[:, 0])
    flip = np.einsum("ij,ij->i", across, normals) < 0
    positions[flip, 1], positions[flip, 2] = positions[flip, 2].copy(), positions[flip, 1].copy()

    materials = meta.get("materials", [])
    doc = {"asset": {"version": "2.0", "generator": "kin tools/reference/studio_mesh_to_glb.py"},
           "scenes": [{"nodes": [0]}], "scene": 0, "nodes": [{"name": meta.get("asset", "model"), "mesh": 0}],
           "meshes": [{"name": meta.get("asset", "model"), "primitives": []}], "materials": [], "accessors": [], "bufferViews": []}
    blob = bytearray()

    def view(data, target):
        while len(blob) % 4:
            blob.append(0)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(doc["bufferViews"]) - 1

    for m, mat in enumerate(materials):
        colour = linear(mat.get("colour", [128, 128, 128]))
        entry = {"name": mat.get("name", f"m{m}"), "doubleSided": True,
                 "pbrMetallicRoughness": {"baseColorFactor": [*colour.tolist(), 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}}
        if mat.get("emit"):
            entry["emissiveFactor"] = colour.tolist()
        doc["materials"].append(entry)
    for m in range(max(len(materials), int(material.max()) + 1 if n else 0)):
        mask = material == m
        count = int(mask.sum())
        if count == 0:
            continue
        p = np.ascontiguousarray(positions[mask].reshape(-1, 3), dtype=np.float32)
        nn = np.ascontiguousarray(np.repeat(normals[mask], 3, axis=0), dtype=np.float32)
        pv = view(p.tobytes(), 34962)
        nv = view(nn.tobytes(), 34962)
        doc["accessors"].append({"bufferView": pv, "componentType": 5126, "count": len(p), "type": "VEC3",
                                 "min": p.min(axis=0).tolist(), "max": p.max(axis=0).tolist()})
        doc["accessors"].append({"bufferView": nv, "componentType": 5126, "count": len(nn), "type": "VEC3"})
        primitive = {"attributes": {"POSITION": len(doc["accessors"]) - 2, "NORMAL": len(doc["accessors"]) - 1}}
        if m < len(materials):
            primitive["material"] = m
        doc["meshes"][0]["primitives"].append(primitive)
    while len(blob) % 4:
        blob.append(0)
    doc["buffers"] = [{"byteLength": len(blob)}]
    text = json.dumps(doc, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942)
    with open(out, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + len(body) + len(blob)))
        f.write(body)
        f.write(blob)
    print(f"{out}: {n} triangles, {len(doc['meshes'][0]['primitives'])} primitives")


if __name__ == "__main__":
    main()
