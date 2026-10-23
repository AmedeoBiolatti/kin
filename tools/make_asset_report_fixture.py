#!/usr/bin/env python3
"""Generate real PNG/WAV assets for the shipping graph audit (no external libs)."""
import argparse
import json
from pathlib import Path
import random
import struct
import wave
import zlib


def create_fixture(output):
    assets = output / "assets"
    assets.mkdir(parents=True, exist_ok=True)

    def write(path, data):
        target = assets / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def png(path, size, seed):
        rng = random.Random(seed)
        raw = b"".join(b"\0" + rng.randbytes(size * 4) for _ in range(size))

        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        write(path, b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    def wav(path, seconds):
        target = assets / path
        target.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(target), "wb") as stream:
            stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            # A deterministic square wave is sufficient to exercise real PCM bytes.
            stream.writeframes(b"".join(struct.pack("<h", 4000 if (i // 60) % 2 else -4000)
                                        for i in range(48000 * seconds)))

    write("scenes/main.json", b'{"catalog":"catalogs/hero.json","audio":"audio/play.wav"}\n')
    write("catalogs/hero.json", b'{"texture":"textures/hero.png"}\n')
    png("textures/hero.png", 256, 17)
    png("unused/old-level.png", 512, 29)
    wav("audio/play.wav", 1)
    wav("unused/old-music.wav", 4)
    for locale in ("en", "fr", "ja", "ar"):
        write(f"locales/{locale}.kinlang", json.dumps({"locale": locale, "title": "Fixture"}).encode())
    write("seasonal/winter.bin", random.Random(43).randbytes(65536))
    dependencies = {"scenes/main.json": ["catalogs/hero.json", "audio/play.wav"],
                    "catalogs/hero.json": ["textures/hero.png"]}
    nodes = {path.relative_to(assets).as_posix(): {"dependencies": dependencies.get(path.relative_to(assets).as_posix(), []),
             "dependencies_complete": True} for path in sorted(assets.rglob("*")) if path.is_file()}
    manifest = {"schema": "kin.asset-manifest/1", "roots_complete": True, "roots": ["scenes/main.json"],
                "dynamic_groups": {"locales": [f"locales/{locale}.kinlang" for locale in ("en", "fr", "ja", "ar")],
                                   "seasonal": ["seasonal/winter.bin"]}, "assets": nodes}
    path = output / "assets.manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return assets, path


def create_dependency_fixture(output):
    assets, manifest = create_fixture(output)
    (assets / "scenes/main.json").unlink()
    (assets / "catalogs/hero.json").unlink()
    (assets / "scripts").mkdir(exist_ok=True)
    (assets / "prefabs").mkdir(exist_ok=True)
    (assets / "scripts/main.lua").write_text("function on_load(scene) end\n")
    (assets / "scenes/main.kinscene").write_text(json.dumps({
        "schema": "kin.scene/1", "script": "scripts/main.lua", "entities": [],
        "prefabs": [{"id": "hero", "asset": "prefabs/hero.kinprefab"}]}))
    (assets / "prefabs/hero.kinprefab").write_text(json.dumps({"schema": "kin.prefab/1",
        "id": "hero", "root": "root", "entities": [{"id": "root", "components": {}}]}))
    (assets / "catalogs/hero.kinsprites").write_text(
        "texture hero textures/hero.png\nsprite hero hero 0 0 256 256\n")
    (assets / "audio/main.kinaudio").write_text(
        "bus master 1\nclip play play.wav\ncue play sound master clips=play\n")
    for source, target in (("textures/hero.png", "l10n/fr/textures/hero.png"),
                           ("audio/play.wav", "audio/l10n/ja/play.wav")):
        destination = assets / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes((assets / source).read_bytes())
    data = json.loads(manifest.read_text())
    data.update(discover_dependencies=True, roots=["scenes/main.kinscene", "audio/main.kinaudio"], assets={
        "scenes/main.kinscene": {"dependencies_complete": True},
        "prefabs/hero.kinprefab": {"dependencies_complete": True},
        "scripts/main.lua": {"dependencies": ["catalogs/hero.kinsprites"], "dependencies_complete": True},
        "seasonal/winter.bin": {"dependencies_complete": True}})
    manifest.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    return assets, manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dependency-tracking", action="store_true", help="Use real Kin scene, prefab and catalog formats")
    args = parser.parse_args()
    (create_dependency_fixture if args.dependency_tracking else create_fixture)(args.output)
