#!/usr/bin/env python3
"""Fetch only Summit's selected CC0 Kenney models; stdlib only, pinned archive."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import urllib.request
import zipfile

SOURCE = "https://kenney.nl/assets/survival-kit"
URL = "https://kenney.nl/media/pages/assets/survival-kit/4065a8185b-1712149243/kenney_survival-kit.zip"
SHA256 = "c3586341b5932c87eb43d75d915434f47daed168b17ed36a03e8ca9977c7443e"
MODELS = ("tent-canvas", "campfire-pit", "bedroll", "bedroll-packed", "box", "barrel", "bottle",
          "resource-wood", "tree", "tree-tall", "rock-a", "rock-b", "grass", "grass-large")
ROOT = Path(__file__).resolve().parents[2] / "examples/summit/content/kenney-survival"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="Use an already downloaded archive (checksum still checked)")
    args = parser.parse_args()
    if args.archive:
        data = args.archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            data = response.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("Archive size/checksum changed; review the upstream pack before updating the pin")
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        selected = {name + ".glb": "Models/GLB format/" + name + ".glb" for name in MODELS}
        selected.update({"Textures/colormap.png": "Models/GLB format/Textures/colormap.png", "License.txt": "License.txt"})
        for name, source in selected.items():
            blob = archive.read(source)
            dest = ROOT / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(blob)
            files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
            if name.endswith(".glb"):
                length = struct.unpack_from("<I", blob, 12)[0]
                gltf = json.loads(blob[20:20 + length])
                # Keep this set self-contained, using exactly the shared atlas below.
                assert all(image["uri"] == "Textures/colormap.png" for image in gltf.get("images", []))
                assert all("uri" not in buffer for buffer in gltf.get("buffers", []))
                files[name]["triangles"] = sum(gltf["accessors"][primitive["indices"]]["count"] // 3
                                                for mesh in gltf["meshes"] for primitive in mesh["primitives"])
    manifest = {"author": "Kenney", "pack": "Survival Kit 2.0", "license": "CC0-1.0", "source": SOURCE,
                "download": URL, "archive_sha256": SHA256, "modifications": "None; selected original files only",
                "files": files}
    (ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Selected {len(MODELS)} models, {sum(f['bytes'] for f in files.values()):,} bytes, "
          f"{sum(f.get('triangles', 0) for f in files.values()):,} unique triangles")


if __name__ == "__main__":
    main()
