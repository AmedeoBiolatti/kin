#!/usr/bin/env python3
"""Import authoring fixtures, inspect their pixels, and validate native loaders."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("kin_import_2d", ROOT / "tools/import_2d_assets.py")
imports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imports)
try:
    from PIL import Image
except ModuleNotFoundError:
    Image = None
VALIDATOR = None


@unittest.skipIf(Image is None, "optional Pillow import dependency is not installed")
class Imports(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kin-import-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.art = self.root / "art"
        self.art.mkdir()
        self.output = self.root / "content"
        image = Image.new("RGBA", (4, 2), (255, 0, 0, 255))
        image.paste((0, 255, 0, 128), (2, 0, 4, 2))
        image.save(self.art / "sheet.png")

    def write(self, name, value):
        path = self.art / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def build(self, jobs):
        path = self.write("imports.json", {"schema": "kin.imports/1", "imports": jobs})
        return imports.build(path, self.output)

    def aseprite(self):
        self.write("hero.json", {"frames": [
            {"filename": "a", "frame": {"x": 0, "y": 0, "w": 2, "h": 2}, "duration": 80,
             "sourceSize": {"w": 4, "h": 3}, "spriteSourceSize": {"x": 1, "y": 1, "w": 2, "h": 2}},
            {"filename": "b", "frame": {"x": 2, "y": 0, "w": 2, "h": 2}, "duration": 120,
             "sourceSize": {"w": 4, "h": 3}, "spriteSourceSize": {"x": 1, "y": 0, "w": 2, "h": 2}}],
            "meta": {"image": "sheet.png", "frameTags": [{"name": "walk", "from": 0, "to": 1, "direction": "reverse"}]}})
        return {"kind": "aseprite", "source": "hero.json", "prefix": "hero", "output": "sprites/hero.kinsprites",
                "texture": "images/hero.png", "padding": 1}

    def tiled(self):
        self.write("tiles.tsj", {"name": "terrain", "tilewidth": 2, "tileheight": 2, "tilecount": 2,
                                 "columns": 2, "image": "sheet.png", "tiles": [{"id": 1,
                                 "properties": [{"name": "solid", "value": True}],
                                 "animation": [{"tileid": 0, "duration": 100}, {"tileid": 1, "duration": 200}]}]})
        self.write("map.tmj", {"orientation": "orthogonal", "width": 2, "height": 1, "tilewidth": 2, "tileheight": 2,
                               "tilesets": [{"firstgid": 7, "source": "tiles.tsj"}], "layers": [
                               {"type": "tilelayer", "name": "ground", "width": 2, "height": 1, "data": [7, 8]},
                               {"type": "objectgroup", "name": "objects", "objects": [
                                   {"id": 1, "class": "Spawn", "x": 3, "y": 1, "point": True}]}]})
        return {"kind": "tiled", "source": "map.tmj", "output": "maps/tiled.kintilemap",
                "entities": {"Spawn": "prefabs/spawn.kinprefab"}}

    def ldtk(self):
        self.write("world.ldtk", {"defs": {"tilesets": [{"uid": 1, "identifier": "terrain", "relPath": "sheet.png",
                                                        "tileGridSize": 2}]}, "levels": [
            {"identifier": "Start", "pxWid": 4, "pxHei": 2, "layerInstances": [
                {"__identifier": "Terrain", "__type": "IntGrid", "__gridSize": 2, "__cWid": 2, "__cHei": 1,
                 "__tilesetDefUid": 1, "autoLayerTiles": [{"px": [0, 0], "t": 0, "f": 0}, {"px": [2, 0], "t": 1, "f": 0}],
                 "intGridCsv": [0, 1]}]}]})
        return {"kind": "ldtk", "source": "world.ldtk", "output": "maps/ldtk.kintilemap",
                "solid_values": {"Terrain": [1]}}

    def test_aseprite_pixels_timing_and_deterministic_rebuild(self):
        job = self.aseprite()
        first = self.build([job])
        catalog = (self.output / job["output"]).read_text()
        self.assertIn("animation hero.walk loop 1000 hero.1*120 hero.0*80", catalog)
        with Image.open(self.output / job["texture"]) as image:
            # Frames are restored to the original canvas before packing.
            self.assertEqual(image.getpixel((1, 1)), (0, 0, 0, 0))
            self.assertEqual(image.getpixel((2, 2)), (255, 0, 0, 255))
        stamp = (self.output / job["texture"]).stat().st_mtime_ns
        self.assertEqual(first, self.build([job]))
        self.assertEqual(stamp, (self.output / job["texture"]).stat().st_mtime_ns)

    def test_atlas_extrusion_preserves_alpha_and_pivot(self):
        job = {"kind": "atlas", "sprites": [{"id": "tile", "source": "sheet.png", "pivot": [0.25, 1]}],
               "output": "atlas.kinsprites", "texture": "atlas.png", "padding": 2}
        self.build([job])
        with Image.open(self.output / "atlas.png") as image:
            self.assertEqual(image.size, (8, 6))
            self.assertEqual(image.getpixel((7, 5)), (0, 255, 0, 128))
            self.assertEqual(image.getpixel((0, 0)), (255, 0, 0, 255))
        self.assertIn("4 2 4 2 0.25 1 0 0", (self.output / "atlas.kinsprites").read_text())

    def test_maps_native_coordinates_animation_and_collision(self):
        self.build([self.tiled(), self.ldtk()])
        tiled = json.loads((self.output / "maps/tiled.kintilemap").read_text())
        self.assertEqual(tiled["layers"][0]["cells"], [[1, 2]])
        self.assertEqual(tiled["tileset"]["tiles"][1]["frames"], [0, 1, 1])
        self.assertEqual(tiled["tileset"]["tiles"][1]["frame_seconds"], 0.1)
        scene = json.loads((self.output / "maps/tiled.kinscene").read_text())
        self.assertEqual(scene["prefabs"][0]["pos"], {"x": 3, "y": 1})
        ldtk = json.loads((self.output / "maps/ldtk.kintilemap").read_text())
        self.assertTrue(ldtk["tileset"]["tiles"][2]["solid"])
        self.assertEqual(ldtk["layers"][1]["cells"], [[0, 3]])

    def test_unsupported_input_keeps_previous_outputs(self):
        job = self.tiled()
        self.build([job])
        before = (self.output / job["output"]).read_bytes()
        path = self.art / "map.tmj"
        data = json.loads(path.read_text())
        data["layers"][0]["data"][0] |= 0x80000000
        self.write("map.tmj", data)
        with self.assertRaisesRegex(imports.ImportError, "flipped/rotated"):
            self.build([job])
        self.assertEqual(before, (self.output / job["output"]).read_bytes())

    def test_output_escape_and_duplicate_ids_rejected(self):
        job = {"kind": "atlas", "sprites": [{"id": "tile", "source": "sheet.png"}],
               "output": "../escape.kinsprites", "texture": "atlas.png"}
        with self.assertRaisesRegex(imports.ImportError, "relative content path"):
            self.build([job])
        self.assertFalse((self.root / "escape.kinsprites").exists())
        job["output"] = "atlas.kinsprites"
        job["sprites"] *= 2
        with self.assertRaisesRegex(imports.ImportError, "duplicate sprite id"):
            self.build([job])

    def test_stale_generated_files_removed_but_unmanaged_files_kept(self):
        job = self.aseprite()
        self.build([job])
        (self.output / "unmanaged.txt").write_text("keep")
        self.build([])
        self.assertFalse((self.output / job["output"]).exists())
        self.assertEqual((self.output / "unmanaged.txt").read_text(), "keep")

    def test_symlink_output_escape_rejected_before_writing(self):
        job = self.aseprite()
        self.output.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        try:
            (self.output / "images").symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("directory symlinks unavailable")
        with self.assertRaisesRegex(imports.ImportError, "symlink escapes"):
            self.build([job])
        self.assertFalse(any(outside.iterdir()))
        self.assertFalse((self.output / "sprites").exists())

    def test_aseprite_cli_exports_and_tracks_only_real_sources(self):
        job = self.aseprite()
        job["source"] = "hero.aseprite"
        (self.art / job["source"]).write_bytes(b"authoring document")

        def export(argv, **kwargs):
            self.assertIn("--batch", argv)
            self.assertLess(argv.index("--list-tags"), argv.index(str(self.art / job["source"])))
            data = json.loads((self.art / "hero.json").read_text())
            Path(argv[argv.index("--data")+1]).write_text(json.dumps(data))
            shutil.copyfile(self.art / "sheet.png", argv[argv.index("--sheet")+1])
            return subprocess.CompletedProcess(argv, 0, "", "")

        with mock.patch.object(imports.shutil, "which", return_value="aseprite"), \
             mock.patch.object(imports.subprocess, "run", side_effect=export):
            report = self.build([job])
        self.assertIn("hero.aseprite", report["dependencies"])
        self.assertFalse(any("kin-aseprite-" in path for path in report["dependencies"]))

    def test_cmake_imports_rebuild_and_ship_generated_content(self):
        if not shutil.which("cmake"): self.skipTest("cmake unavailable")
        job = self.aseprite()
        manifest = self.write("imports.json", {"schema": "kin.imports/1", "imports": [job]})
        project = self.root / "project"
        project.mkdir()
        (project / "content").mkdir()
        (project / "content/readme.txt").write_text("game content")
        (project / "main.cpp").write_text("int main() { return 0; }\n")
        (project / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.22)
project(import_check LANGUAGES CXX)
list(APPEND CMAKE_MODULE_PATH "{ROOT.as_posix()}/cmake")
include(kin_release)
include(kin_game)
function(kin_enable_release_optimizations target)
endfunction()
function(kin_enable_release_link_optimizations target)
endfunction()
add_executable(check main.cpp)
kin_game(check NAME imported IMPORTS "{manifest.as_posix()}")
''')
        build = self.root / "build"

        def run(*args):
            result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)

        run("cmake", "-S", str(project), "-B", str(build), "-DKIN_PACK_CONTENT=OFF", "-DCMAKE_BUILD_TYPE=Release")
        run("cmake", "--build", str(build), "--target", "check_package")
        stage = next(path for path in (build / "packages").iterdir() if path.is_dir())
        self.assertEqual((stage / "imported/readme.txt").read_text(), "game content")
        self.assertTrue((stage / "imported" / job["output"]).is_file())
        self.assertFalse((stage / "imported/.kin-import-report.json").exists())
        # An image edit rebuilds the import without a manual configure.
        Image.new("RGBA", (4, 2), (0, 0, 255, 255)).save(self.art / "sheet.png")
        run("cmake", "--build", str(build), "--target", "check_package")
        with Image.open(stage / "imported" / job["texture"]) as image:
            self.assertEqual(image.getpixel((2, 2)), (0, 0, 255, 255))
        # A changed manifest changes the output inventory and removes stale assets.
        old_output = job["output"]
        job["output"] = "sprites/renamed.kinsprites"
        self.write("imports.json", {"schema": "kin.imports/1", "imports": [job]})
        run("cmake", "--build", str(build), "--target", "check_package")
        self.assertTrue((stage / "imported" / job["output"]).exists())
        self.assertFalse((build / "imported-content/check" / old_output).exists())

    def test_native_loaders_consume_generated_assets(self):
        if not VALIDATOR: self.skipTest("native validator is supplied by CTest")
        self.build([self.aseprite(), self.tiled(), self.ldtk()])
        subprocess.run([VALIDATOR, str(self.output)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--validator")
    args, remaining = parser.parse_known_args()
    VALIDATOR = args.validator
    unittest.main(argv=[sys.argv[0]] + remaining)
