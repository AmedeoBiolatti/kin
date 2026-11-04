#!/usr/bin/env python3
"""Build native Kin 2D assets from an explicit, reproducible import manifest.

Only this build tool needs Pillow. Games consume ordinary PNG/.kinsprites/
.kintilemap/.kinscene files and gain no runtime dependency on the authoring tools.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import time


class ImportError(ValueError):
    pass


class ImportCache:
    """Content-addressed, verified job outputs; source hashes determine validity."""
    def __init__(self, root):
        self.root = Path(root)
        self.tool_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        self.hits = self.misses = 0

    def key(self, job, source_root, aseprite):
        Image = pillow()
        executable = shutil.which(aseprite) if job.get("source", "").endswith((".ase", ".aseprite")) else None
        exporter = hashlib.sha256(Path(executable).read_bytes()).hexdigest() if executable and Path(executable).is_file() else aseprite
        return hashlib.sha256(json_bytes({"job": job, "root": str(source_root), "tool": self.tool_hash,
                                         "pillow": Image.__version__, "aseprite": exporter})).hexdigest()

    def read(self, key):
        try:
            entry = json.loads((self.root / (key + ".json")).read_text())
            for name, digest in entry["dependencies"].items():
                if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest: return None
            files = {}
            for name, digest in entry["files"].items():
                data = (self.root / "objects" / digest).read_bytes()
                if hashlib.sha256(data).hexdigest() != digest: return None
                files[output_path(name)] = data
            return files, {Path(name): digest for name, digest in entry["dependencies"].items()}
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def write(self, key, pipeline):
        entry = {"dependencies": {}, "files": {}}
        for path, digest in sorted(pipeline.fingerprints.items()):
            entry["dependencies"][str(path)] = digest
        for name, data in pipeline.files.items():
            digest = hashlib.sha256(data).hexdigest()
            entry["files"][name] = digest
            atomic_write(self.root / "objects" / digest, data)
        atomic_write(self.root / (key + ".json"), json_bytes(entry))


def require(condition, message):
    if not condition:
        raise ImportError(message)


def token(value):
    require(isinstance(value, str) and value and not any(c.isspace() or c in "#*" for c in value),
            f"native catalog ids/paths cannot contain whitespace, # or *: {value!r}")
    return value


def output_path(value):
    path = PurePosixPath(value)
    require(value and path.parts and not path.is_absolute() and ".." not in path.parts and "\\" not in value and ":" not in value,
            f"output must be a relative content path: {value!r}")
    return path.as_posix()


def integer(value, name, minimum=0, maximum=65535):
    require(type(value) is int and minimum <= value <= maximum, f"{name}: expected integer {minimum}..{maximum}")
    return value


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def png_bytes(image):
    result = io.BytesIO()
    image.save(result, format="PNG", compress_level=9)
    return result.getvalue()


def pillow():
    try:
        from PIL import Image
    except ModuleNotFoundError as error:
        raise ImportError("2D imports need Pillow: install tools/requirements-imports.txt") from error
    return Image


def image_file(path, data=None):
    Image = pillow()
    with Image.open(io.BytesIO(data) if data is not None else path) as image:
        require(image.width <= 16384 and image.height <= 16384 and image.width * image.height <= 67108864,
                f"image is too large: {path}")
        return image.convert("RGBA")


def atomic_write(path, data):
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".kin-import-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class Pipeline:
    def __init__(self, root, aseprite="aseprite"):
        self.root = Path(root).resolve()
        self.files = {}
        self.dependencies = set()
        self.fingerprints = {}
        self.images = {}
        self.aseprite_executable = aseprite

    def source(self, path):
        resolved = (self.root / path).resolve()
        self.read_bytes(resolved)
        return resolved

    def read_bytes(self, path):
        path = Path(path).resolve()
        self.dependencies.add(path)
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        require(path not in self.fingerprints or self.fingerprints[path] == digest,
                f"source changed while importing; retry: {path}")
        self.fingerprints[path] = digest
        return data

    def verify(self):
        for path, digest in self.fingerprints.items():
            require(hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                    f"source changed while importing; retry: {path}")

    def image(self, path):
        path = Path(path).resolve()
        if path not in self.images: self.images[path] = image_file(path, self.read_bytes(path))
        return self.images[path]

    def read_json(self, path):
        value = json.loads(self.read_bytes(path).decode("utf-8-sig"))
        require(isinstance(value, dict), f"expected JSON object: {path}")
        return value

    def emit(self, path, data):
        path = output_path(path)
        require(path not in self.files, f"duplicate import output: {path}")
        self.files[path] = data

    def pack(self, sprites, output, texture, maximum=2048, padding=2, animations=()):
        """Deterministic shelf packing, without rotation or lossy alpha trimming."""
        Image = pillow()
        integer(maximum, "max_size", 1, 8192)
        integer(padding, "padding", 0, 32)
        require(sprites, "atlas has no sprites")
        ids = [token(s["id"]) for s in sprites]
        require(len(set(ids)) == len(ids), "duplicate sprite id")
        require(maximum * maximum <= 67108864, "atlas exceeds pixel budget")
        ordered = sorted(sprites, key=lambda s: (-s["image"].height, -s["image"].width, s["id"]))
        x = y = row_height = used_width = 0
        placements = {}
        for sprite in ordered:
            image = sprite["image"]
            width, height = image.width + 2 * padding, image.height + 2 * padding
            require(width <= maximum and height <= maximum, f"sprite {sprite['id']!r} exceeds atlas size")
            if x + width > maximum:
                y += row_height
                x = row_height = 0
            require(y + height <= maximum, "atlas is full; increase max_size or split the manifest")
            placements[sprite["id"]] = (x + padding, y + padding)
            x += width
            used_width = max(used_width, x)
            row_height = max(row_height, height)
        atlas = Image.new("RGBA", (max(1, used_width), max(1, y + row_height)))
        texture = token(output_path(texture))
        lines = ["# Generated by import_2d_assets.py", f"texture atlas {texture}"]
        for sprite in sorted(sprites, key=lambda s: s["id"]):
            image = sprite["image"]
            x, y = placements[sprite["id"]]
            w, h = image.size
            atlas.paste(image, (x, y))
            # Extrude all edges, including corners, to prevent linear-filter bleed.
            if padding:
                for box, dest in [((0, 0, w, 1), (x, y-padding, w, padding)),
                                  ((0, h-1, w, h), (x, y+h, w, padding)),
                                  ((0, 0, 1, h), (x-padding, y, padding, h)),
                                  ((w-1, 0, w, h), (x+w, y, padding, h))]:
                    dx, dy, dw, dh = dest
                    atlas.paste(image.crop(box).resize((dw, dh), Image.Resampling.NEAREST), (dx, dy))
                for sx, sy, dx, dy in [(0,0,x-padding,y-padding), (w-1,0,x+w,y-padding),
                                       (0,h-1,x-padding,y+h), (w-1,h-1,x+w,y+h)]:
                    atlas.paste(image.getpixel((sx, sy)), (dx, dy, dx+padding, dy+padding))
            pivot = sprite.get("pivot", [0.5, 0.5])
            require(len(pivot) == 2 and all(isinstance(v, (int, float)) and math.isfinite(v) for v in pivot),
                    "pivot must contain two finite numbers")
            lines.append(f"sprite {sprite['id']} atlas {x} {y} {w} {h} {w} {h} {pivot[0]:.9g} {pivot[1]:.9g} 0 0")
        lines.extend(animations)
        self.emit(texture, png_bytes(atlas))
        self.emit(output, ("\n".join(lines) + "\n").encode())

    def atlas(self, job):
        sprites = []
        for entry in job["sprites"]:
            sprites.append({"id": entry["id"], "image": self.image(self.source(entry["source"])),
                            "pivot": entry.get("pivot", [0.5, 0.5])})
        self.pack(sprites, job["output"], job["texture"], job.get("max_size", 2048), job.get("padding", 2))

    def aseprite(self, job):
        source = self.source(job["source"])
        if source.suffix.lower() in (".ase", ".aseprite"):
            executable = shutil.which(self.aseprite_executable)
            require(executable, "Aseprite executable not found; install it or import a JSON+PNG export")
            with tempfile.TemporaryDirectory(prefix="kin-aseprite-") as temporary:
                data = Path(temporary) / "sheet.json"
                sheet = Path(temporary) / "sheet.png"
                result = subprocess.run([executable, "--batch", "--list-tags", "--list-slices", str(source),
                                         "--format", "json-array", "--data", str(data), "--sheet", str(sheet)],
                                        capture_output=True, text=True)
                require(result.returncode == 0, "Aseprite export failed: " + result.stderr.strip())
                self._aseprite(job, self.read_json(data), sheet)
                ephemeral = Path(temporary).resolve()
                self.dependencies = {p for p in self.dependencies if not p.is_relative_to(ephemeral)}
                self.fingerprints = {p: digest for p, digest in self.fingerprints.items() if not p.is_relative_to(ephemeral)}
        else:
            data = self.read_json(source)
            self._aseprite(job, data, source.parent / data["meta"]["image"])

    def _aseprite(self, job, data, sheet_path):
        Image = pillow()
        sheet = self.image(sheet_path)
        frames = data["frames"]
        frames = list(frames.values()) if isinstance(frames, dict) else frames
        require(frames, "Aseprite export has no frames")
        prefix = token(job.get("prefix", "sprite"))
        sprites, durations = [], []
        for index, frame in enumerate(frames):
            require(not frame.get("rotated"), "rotated Aseprite frames are unsupported; export without rotation")
            rect, original = frame["frame"], frame["sourceSize"]
            x, y, w, h = (integer(rect[k], f"frame.{k}", 0 if k in ("x", "y") else 1, 16384) for k in ("x", "y", "w", "h"))
            require(x+w <= sheet.width and y+h <= sheet.height, "Aseprite frame exceeds its sheet")
            ow, oh = integer(original["w"], "source width", 1, 16384), integer(original["h"], "source height", 1, 16384)
            require(ow * oh <= 67108864, "Aseprite source canvas exceeds pixel budget")
            trim = frame.get("spriteSourceSize", {"x": 0, "y": 0, "w": w, "h": h})
            tx, ty = integer(trim["x"], "trim x"), integer(trim["y"], "trim y")
            require(trim["w"] == w and trim["h"] == h and tx+w <= ow and ty+h <= oh, "invalid Aseprite trim rectangle")
            canvas = Image.new("RGBA", (ow, oh))
            canvas.paste(sheet.crop((x, y, x+w, y+h)), (tx, ty))
            pivot = job.get("pivot", [0.5, 0.5])
            if "slice" in job:
                matches = [s for s in data.get("meta", {}).get("slices", []) if s["name"] == job["slice"]]
                require(len(matches) == 1, "requested Aseprite pivot slice was not found")
                keys = [k for k in matches[0]["keys"] if k["frame"] <= index]
                require(keys, "Aseprite slice has no key for this frame")
                key = max(keys, key=lambda k: k["frame"])
                require("pivot" in key, "Aseprite slice has no pivot for this frame")
                pivot = [(key["bounds"]["x"] + key["pivot"]["x"]) / ow,
                         (key["bounds"]["y"] + key["pivot"]["y"]) / oh]
            sprites.append({"id": f"{prefix}.{index}", "image": canvas, "pivot": pivot})
            durations.append(integer(frame["duration"], "frame duration (ms)", 1, 3600000))
        tags = data.get("meta", {}).get("frameTags", []) or [{"name": "default", "from": 0, "to": len(frames)-1}]
        animations, names = [], set()
        for tag in tags:
            start = integer(tag["from"], "tag start", 0, len(frames)-1)
            end = integer(tag["to"], "tag end", start, len(frames)-1)
            direction = tag.get("direction", "forward")
            require(direction in ("forward", "reverse", "pingpong", "pingpong_reverse"), "unsupported Aseprite tag direction")
            order = list(range(start, end+1))
            if direction in ("reverse", "pingpong_reverse"): order.reverse()
            playback = "pingpong" if direction.startswith("pingpong") else "loop"
            repeat = int(tag.get("repeat") or 0)
            integer(repeat, "tag repeat", 0, 65535)
            if repeat:
                if playback == "pingpong": order += order[-2:0:-1]
                require(len(order) * repeat <= 65535, "Aseprite repeated tag exceeds frame budget")
                order *= repeat
                playback = "once"
            tokens = " ".join(f"{prefix}.{i}*{durations[i]}" for i in order)
            name = token(prefix + '.' + tag['name'])
            require(name not in names, "duplicate Aseprite animation tag")
            names.add(name)
            animations.append(f"animation {name} {playback} 1000 {tokens}")
        self.pack(sprites, job["output"], job["texture"], job.get("max_size", 2048), job.get("padding", 2), animations)

    def run(self, manifest, cache=None):
        require(manifest.get("schema") == "kin.imports/1", "expected schema kin.imports/1")
        jobs = manifest.get("imports")
        require(isinstance(jobs, list), "imports must be an array")
        for index, job in enumerate(jobs):
            require(isinstance(job, dict), f"import {index}: expected object")
            try:
                kind = job.get("kind")
                require(kind in ("atlas", "aseprite", "tiled", "ldtk"), f"unknown import kind: {kind}")
                key = cache.key(job, self.root, self.aseprite_executable) if cache else None
                cached = cache.read(key) if cache else None
                if cached is not None:
                    cache.hits += 1
                    files, fingerprints = cached
                    dependencies = set(fingerprints)
                else:
                    child = Pipeline(self.root, self.aseprite_executable)
                    getattr(child, kind)(job)
                    child.verify()
                    files, dependencies = child.files, child.dependencies
                    fingerprints = child.fingerprints
                    if cache:
                        cache.misses += 1
                        cache.write(key, child)
                for name, data in files.items(): self.emit(name, data)
                self.dependencies.update(dependencies)
                for path, digest in fingerprints.items():
                    require(path not in self.fingerprints or self.fingerprints[path] == digest,
                            f"source changed between import jobs; retry: {path}")
                self.fingerprints.update(fingerprints)
            except (KeyError, TypeError, ValueError, OSError) as error:
                raise ImportError(f"import {index} ({job.get('source', job.get('output', '?'))}): {error}") from error

    def _emit_map(self, job, cols, rows, tw, th, tiles, layers, prefabs, grid=None):
        Image = pillow()
        require(cols * rows <= 4194304, "map exceeds 4 million cells")
        require(tiles and len(tiles) <= 65535, "map must contain 1..65535 tile definitions")
        output = output_path(job["output"])
        require(output.endswith(".kintilemap"), "map output must end in .kintilemap")
        texture = output_path(job.get("texture", str(PurePosixPath(output).with_suffix(".png"))))
        columns = min(len(tiles), max(1, 2048 // tw))
        height = math.ceil(len(tiles) / columns) * th
        require(columns * tw <= 8192 and height <= 8192 and columns * tw * height <= 67108864,
                "combined tile sheet is too large; split the map/tilesets")
        atlas = Image.new("RGBA", (columns * tw, height))
        definitions = []
        for index, (definition, image) in enumerate(tiles):
            definition = dict(definition, tile_id=index+1)
            if image is not None:
                require(image.size == (tw, th), "tiles must match the map's cell size")
                atlas.paste(image, ((index % columns) * tw, (index // columns) * th))
                definition["frame"] = index
                definition["color"] = {"r": 255, "g": 255, "b": 255, "a": 255}
            definitions.append(definition)
        tilemap = {"format": "kin.tilemap", "version": 1, "size": {"cols": cols, "rows": rows},
                   "tile_size": {"w": tw, "h": th}, "layers": layers,
                   "tileset": {"image": texture, "frame_size": {"w": tw, "h": th}, "tiles": definitions}}
        if grid: tilemap["grid"] = grid
        self.emit(texture, png_bytes(atlas))
        self.emit(output, json_bytes(tilemap))
        components = {"Transform2D": {}, "TilemapComponent": {"asset": output}}
        if any(d.get("solid") for d in definitions): components["TilemapCollider"] = {}
        scene = {"schema": "kin.scene/1", "name": job.get("name", PurePosixPath(output).stem),
                 "entities": [{"id": "tilemap", "name": "Tilemap", "components": components}], "prefabs": prefabs}
        self.emit(str(PurePosixPath(output).with_suffix(".kinscene")), json_bytes(scene))

    @staticmethod
    def _properties(properties):
        result = {}
        for entry in properties:
            value = entry.get("value")
            require(isinstance(value, (str, bool, int, float)), "only scalar custom properties are supported")
            result[entry["name"]] = value
        return result

    @staticmethod
    def _prefab(job, identifier, entity_id, x, y):
        prefab = job.get("entities", {}).get(identifier)
        require(prefab, f"entity {identifier!r} needs an entry in the import's entities-to-prefabs mapping")
        return {"id": entity_id, "asset": output_path(prefab), "name": identifier,
                "pos": {"x": x, "y": y}}

    def tiled(self, job):
        source = self.source(job["source"])
        data = self.read_json(source)
        require(not data.get("infinite"), "infinite Tiled maps are unsupported; export a finite map")
        orientation = data.get("orientation", "orthogonal")
        require(orientation in ("orthogonal", "hexagonal"), "only orthogonal and regular hexagonal Tiled maps are supported")
        require(data.get("renderorder", "right-down") == "right-down", "only right-down Tiled render order is supported")
        cols = integer(data["width"], "map width", 1)
        rows = integer(data["height"], "map height", 1)
        require(cols * rows <= 4194304, "map exceeds 4 million cells")
        tw = integer(data["tilewidth"], "tile width", 1, 8192)
        th = integer(data["tileheight"], "tile height", 1, 8192)
        grid = None
        if orientation == "hexagonal":
            axis = data.get("staggeraxis", "y")
            require(axis in ("x", "y"), "invalid Tiled stagger axis")
            require(data.get("hexsidelength", 0) * 2 == (th if axis == "y" else tw), "only regular half-side Tiled hexagons are supported")
            require(data.get("staggerindex", "odd") in ("odd", "even"), "invalid Tiled stagger index")
            grid = {"shape": "hex", "orientation": "pointy" if axis == "y" else "flat",
                    "offset": data.get("staggerindex", "odd")}
        tiles, gids, pending_animations = [], {}, []
        for reference in sorted(data.get("tilesets", []), key=lambda t: t["firstgid"]):
            first = integer(reference["firstgid"], "firstgid", 1, 0x0FFFFFFF)
            tileset, base = reference, source.parent
            if "source" in reference:
                path = (base / reference["source"]).resolve()
                require(path.suffix.lower() in (".json", ".tsj"), "external Tiled tilesets must be JSON (.tsj), not TSX")
                tileset, base = self.read_json(path), path.parent
            require("image" in tileset, "Tiled image-collection tilesets are unsupported; use a sheet")
            require(tileset["tilewidth"] == tw and tileset["tileheight"] == th, "Tiled tilesets must match map cell dimensions")
            require(not tileset.get("tileoffset", {}).get("x") and not tileset.get("tileoffset", {}).get("y"), "tileset offsets are unsupported")
            image = self.image(base / tileset["image"])
            count = integer(tileset["tilecount"], "tile count", 1)
            columns = integer(tileset["columns"], "tile columns", 1)
            margin = integer(tileset.get("margin", 0), "tileset margin")
            spacing = integer(tileset.get("spacing", 0), "tileset spacing")
            authored = {tile["id"]: tile for tile in tileset.get("tiles", [])}
            for index in range(count):
                require(first+index not in gids, "Tiled tileset GID ranges overlap")
                x = margin + (index % columns) * (tw + spacing)
                y = margin + (index // columns) * (th + spacing)
                require(x+tw <= image.width and y+th <= image.height, "Tiled tile exceeds its sheet")
                tile = authored.get(index, {})
                properties = self._properties(tile.get("properties", []))
                definition = {"id": f"{tileset.get('name', 'tiles')}.{index}", "metadata": properties,
                              "solid": bool(properties.get("solid", False)), "cost": properties.get("cost", 10),
                              "blocks_sight": bool(properties.get("blocks_sight", False))}
                integer(definition["cost"], "tile cost", 0, 2147483647)
                objects = tile.get("objectgroup", {}).get("objects", [])
                if objects:
                    require(len(objects) == 1, "a tile may have one collision polygon/rectangle")
                    shape = objects[0]
                    require(not shape.get("rotation") and not shape.get("ellipse") and not shape.get("polyline") and not shape.get("point"),
                            "tile collision must be an unrotated polygon or rectangle")
                    points = shape.get("polygon", [{"x": 0, "y": 0}, {"x": shape.get("width", 0), "y": 0},
                                                   {"x": shape.get("width", 0), "y": shape.get("height", 0)}, {"x": 0, "y": shape.get("height", 0)}])
                    require(len(points) >= 3, "collision polygon needs at least three points")
                    definition["collision_polygon"] = [{"x": p["x"]+shape.get("x", 0), "y": p["y"]+shape.get("y", 0)} for p in points]
                    definition["solid"] = True
                if tile.get("animation"): pending_animations.append((definition, first, tile["animation"]))
                gids[first+index] = len(tiles)+1
                tiles.append((definition, image.crop((x, y, x+tw, y+th))))
        for definition, first, animation in pending_animations:
            durations = [integer(f["duration"], "tile frame milliseconds", 1, 3600000) for f in animation]
            quantum = math.gcd(*durations)
            require(sum(d // quantum for d in durations) <= 4096, "tile animation exceeds 4096 expanded frames")
            definition["frames"] = []
            for frame, duration in zip(animation, durations):
                require(first+frame["tileid"] in gids, "tile animation references an unknown frame")
                definition["frames"].extend([gids[first+frame["tileid"]]-1] * (duration // quantum))
            definition["frame_seconds"] = quantum / 1000
        layers, prefabs = [], []
        names = set()
        for layer in data.get("layers", []):
            require(not layer.get("offsetx") and not layer.get("offsety") and not layer.get("x") and not layer.get("y"), "Tiled layer offsets are unsupported")
            require(layer.get("opacity", 1) == 1 and layer.get("parallaxx", 1) == 1 and layer.get("parallaxy", 1) == 1,
                    "Tiled opacity/parallax require authored Kin components")
            require(not layer.get("tintcolor") and layer.get("mode", "normal") == "normal",
                    "Tiled tint/blend modes require authored Kin components")
            if layer["type"] == "objectgroup":
                for obj in layer.get("objects", []):
                    require(obj.get("point") and not obj.get("gid") and not obj.get("rotation") and not obj.get("width") and not obj.get("height"),
                            "object layers currently support point markers mapped to prefabs")
                    require(not obj.get("properties"), "map object properties must be authored on the mapped prefab")
                    identifier = obj.get("class") or obj.get("type") or obj.get("name")
                    prefabs.append(self._prefab(job, identifier, f"object_{obj['id']}", obj["x"], obj["y"]))
                continue
            require(layer["type"] == "tilelayer", f"unsupported Tiled layer type: {layer['type']}")
            require(layer["name"] not in names, "duplicate Tiled layer name")
            names.add(layer["name"])
            require(layer.get("width") == cols and layer.get("height") == rows, "tile layers must match map dimensions")
            cells = layer.get("data")
            require(isinstance(cells, list) and len(cells) == cols*rows, "Tiled layer must contain an uncompressed JSON tile array")
            native = []
            for gid in cells:
                integer(gid, "tile GID", 0, 0xFFFFFFFF)
                require(not gid & 0xF0000000, "flipped/rotated Tiled cells are unsupported; bake them as distinct tiles")
                require(gid == 0 or gid in gids, f"unknown Tiled GID: {gid}")
                native.append(gids.get(gid, 0))
            properties = self._properties(layer.get("properties", []))
            layers.append({"id": layer["name"], "order": len(layers), "visible": layer.get("visible", True),
                           "collision": properties.get("collision", True), "navigation": properties.get("navigation", True),
                           "cells": [native[row*cols:(row+1)*cols] for row in range(rows)]})
        self._emit_map(job, cols, rows, tw, th, tiles, layers, prefabs, grid)

    def ldtk(self, job):
        source = self.source(job["source"])
        data = self.read_json(source)
        levels = data.get("levels", [])
        if "level" in job: levels = [level for level in levels if level["identifier"] == job["level"]]
        require(len(levels) == 1, "select exactly one LDtk level with the level option")
        level = levels[0]
        if level.get("externalRelPath"):
            level = self.read_json(source.parent / level["externalRelPath"])
        instances = level.get("layerInstances")
        require(isinstance(instances, list) and instances, "LDtk level has no layer instances")
        grid_sizes = {layer["__gridSize"] for layer in instances if layer["__type"] != "Entities"}
        require(len(grid_sizes) == 1, "LDtk tile layers must share one grid size")
        size = integer(next(iter(grid_sizes)), "LDtk grid size", 1, 8192)
        require(level["pxWid"] % size == 0 and level["pxHei"] % size == 0, "LDtk level dimensions must be grid aligned")
        cols, rows = level["pxWid"] // size, level["pxHei"] // size
        integer(cols, "LDtk columns", 1); integer(rows, "LDtk rows", 1)
        require(cols*rows <= 4194304, "map exceeds 4 million cells")
        definitions = {entry["uid"]: entry for entry in data.get("defs", {}).get("tilesets", [])}
        tiles, tile_ids, layers, prefabs = [], {}, [], []

        def tile_id(uid, index):
            key = (uid, index)
            if key in tile_ids: return tile_ids[key]
            require(uid in definitions, f"unknown LDtk tileset: {uid}")
            definition = definitions[uid]
            require(definition["tileGridSize"] == size, "LDtk tileset must match layer grid size")
            image = self.image(source.parent / definition["relPath"])
            spacing, margin = definition.get("spacing", 0), definition.get("padding", 0)
            columns = (image.width - 2*margin + spacing) // (size+spacing)
            require(columns > 0 and type(index) is int and index >= 0, "invalid LDtk tile index")
            x, y = margin+(index % columns)*(size+spacing), margin+(index // columns)*(size+spacing)
            require(x+size <= image.width and y+size <= image.height, "LDtk tile exceeds its sheet")
            tile_ids[key] = len(tiles)+1
            tiles.append(({"id": f"{definition['identifier']}.{index}"}, image.crop((x,y,x+size,y+size))))
            return tile_ids[key]

        for layer in reversed(instances): # LDtk lists the topmost layer first
            require(layer["__type"] in ("Tiles", "AutoLayer", "IntGrid", "Entities"), "unsupported LDtk layer type")
            require(not layer.get("__pxTotalOffsetX") and not layer.get("__pxTotalOffsetY"), "LDtk layer offsets are unsupported")
            require(layer.get("__opacity", 1) == 1, "LDtk layer opacity requires an authored Kin component")
            if layer["__type"] == "Entities":
                for entity in layer.get("entityInstances", []):
                    require(not entity.get("fieldInstances"), "LDtk custom entity fields must be authored on the mapped prefab")
                    prefabs.append(self._prefab(job, entity["__identifier"], "entity_"+entity["iid"], *entity["px"]))
                continue
            require(layer["__cWid"] == cols and layer["__cHei"] == rows, "LDtk tile layers must match level dimensions")
            uid = layer.get("__tilesetDefUid")
            batches = []
            for tile in layer.get("gridTiles", []) + layer.get("autoLayerTiles", []):
                require(tile.get("f", 0) == 0, "flipped LDtk tiles are unsupported; bake distinct tiles")
                require(tile.get("a", 1) == 1, "LDtk per-tile opacity requires an authored Kin component")
                x, y = tile["px"]
                require(x % size == 0 and y % size == 0 and 0 <= x < cols*size and 0 <= y < rows*size,
                        "LDtk tile is outside its grid")
                offset = (y // size)*cols + x // size
                value = tile_id(uid, tile["t"])
                for cells in batches:
                    if not cells[offset]: cells[offset] = value; break
                else:
                    cells = [0]*(cols*rows); cells[offset] = value; batches.append(cells)
            for index, cells in enumerate(batches):
                layers.append({"id": f"{layer['__identifier']}.{index}", "order": len(layers),
                               "visible": layer.get("visible", True), "collision": False, "navigation": False,
                               "cells": [cells[r*cols:(r+1)*cols] for r in range(rows)]})
            values = layer.get("intGridCsv", [])
            if values:
                require(len(values) == cols*rows, "invalid LDtk IntGrid dimensions")
                ids = {}
                solids = job.get("solid_values", {}).get(layer["__identifier"], [])
                for value in sorted(set(values) - {0}):
                    integer(value, "LDtk IntGrid value", 1)
                    ids[value] = len(tiles)+1
                    tiles.append(({"id": f"{layer['__identifier']}.value.{value}", "solid": value in solids,
                                   "metadata": {"int_grid_value": value}}, None))
                cells = [ids.get(value, 0) for value in values]
                layers.append({"id": layer["__identifier"]+".intgrid", "order": len(layers), "visible": False,
                               "collision": True, "navigation": True,
                               "cells": [cells[r*cols:(r+1)*cols] for r in range(rows)]})
        self._emit_map(job, cols, rows, size, size, tiles, layers, prefabs)


@contextmanager
def output_lock(output):
    """Serialize configure/build/watch publishers; the OS releases crash locks."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".kin-import.lock").open("a+b") as stream:
        if stream.seek(0, 2) == 0:
            stream.write(b"\0")
            stream.flush()
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            acquire = lambda: msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            release = lambda: msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            acquire = lambda: fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            release = lambda: fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        deadline = time.monotonic() + 30
        while True:
            try:
                acquire()
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("another import still owns the output directory")
                time.sleep(0.05)
        try:
            yield
        finally:
            stream.seek(0)
            release()


def build(manifest, output, content=None, aseprite="aseprite", cache_dir=None, statistics=None):
    with output_lock(Path(output).resolve()):
        return _build_locked(manifest, output, content, aseprite, cache_dir, statistics)


def _build_locked(manifest, output, content=None, aseprite="aseprite", cache_dir=None, statistics=None):
    manifest, output = Path(manifest).resolve(), Path(output).resolve()
    require(output != manifest.parent, "use a dedicated output directory, separate from import sources")
    pipeline = Pipeline(manifest.parent, aseprite)
    cache = ImportCache(cache_dir if cache_dir is not None else output / ".kin-import-cache")
    if cache_dir is None:
        require(cache.root.resolve().is_relative_to(output), "cache symlink escapes the output directory")
    pipeline.run(pipeline.read_json(manifest), cache)
    if content:
        content = Path(content).resolve()
        require(output != content and not output.is_relative_to(content), "import output must be outside source content")
        for path in sorted(content.rglob("*")):
            if path.is_file() and not any(p.startswith(".") for p in path.relative_to(content).parts):
                pipeline.emit(path.relative_to(content).as_posix(), pipeline.read_bytes(path))
    report_path = output / ".kin-import-report.json"
    old = json.loads(report_path.read_text()).get("files", {}) if report_path.exists() else {}
    report = {"schema": "kin.import-report/1", "files": {
        name: hashlib.sha256(data).hexdigest() for name, data in sorted(pipeline.files.items())}}
    report["dependencies"] = sorted(os.path.relpath(path, manifest.parent) for path in pipeline.dependencies
                                    if path.exists() and not str(path).startswith(tempfile.gettempdir()+"/kin-aseprite-"))
    # Resolve existing parent symlinks before writing or deleting anything.
    for name in set(old) | set(pipeline.files):
        require((output / output_path(name)).resolve().is_relative_to(output), "output symlink escapes the content directory")
    # Validate the entire import before touching any previous output. Each file
    # replacement is atomic, and unchanged outputs retain their modification time.
    pipeline.verify()
    # A live development session retains its immutable active generation while
    # this marker exists. Failed publication remains blocked until a successful
    # import repairs every output and publishes its report.
    publishing = output / ".kin-import-publishing"
    atomic_write(publishing, b"kin.import-publication/1\n")
    for name, data in sorted(pipeline.files.items()): atomic_write(output / name, data)
    for name in sorted(set(old) - set(pipeline.files)):
        stale = output / output_path(name)
        if stale.is_file(): stale.unlink()
    atomic_write(report_path, json_bytes(report))
    publishing.unlink()
    if statistics is not None: statistics.update(hits=cache.hits, misses=cache.misses)
    return report


class ImportWatcher:
    """Debounced authoring watcher. Failed imports keep all last-good outputs."""
    def __init__(self, manifest, output, content=None, aseprite="aseprite", cache_dir=None, debounce=0.1):
        self.manifest, self.output = Path(manifest).resolve(), Path(output).resolve()
        self.content = Path(content).resolve() if content else None
        self.aseprite, self.cache_dir = aseprite, cache_dir
        self.debounce = max(0, debounce)
        self.signature = None
        self.pending_since = None
        self.last_error = None

    def scan(self):
        result = {}
        roots = [self.manifest.parent] + ([self.content] if self.content else [])
        paths = {self.manifest}
        # Include missing/external authoring references even before the first
        # successful build, so creating a missing PNG/tileset retries the job.
        pending = [self.manifest]
        visited = set()
        while pending and len(visited) < 256:
            document = pending.pop()
            if document in visited: continue
            visited.add(document)
            try:
                def references(value):
                    if isinstance(value, dict):
                        for key, child in value.items():
                            if key in ("source", "image", "relPath", "externalRelPath") and isinstance(child, str):
                                path = (document.parent / child).resolve()
                                paths.add(path)
                                if path.suffix.lower() in (".json", ".tmj", ".tsj", ".ldtk", ".ldtkl"): pending.append(path)
                            references(child)
                    elif isinstance(value, list):
                        for child in value: references(child)
                references(json.loads(document.read_text(encoding="utf-8-sig")))
            except (OSError, ValueError): pass
        for root in roots:
            for directory, folders, files in os.walk(root):
                folders[:] = [name for name in folders if not name.startswith(".") and
                              not (Path(directory) / name).resolve().is_relative_to(self.output)]
                for name in files:
                    if not name.startswith("."): paths.add(Path(directory) / name)
        # External referenced files can live outside the authoring folder.
        try:
            report = json.loads((self.output / ".kin-import-report.json").read_text())
            paths.update((self.manifest.parent / name).resolve() for name in report["dependencies"])
        except (OSError, ValueError, KeyError): pass
        for path in paths:
            try:
                stat = path.stat()
                result[str(path)] = (stat.st_mtime_ns, stat.st_size)
            except OSError: result[str(path)] = None
        return result

    def poll(self, now=None):
        now = time.monotonic() if now is None else now
        signature = self.scan()
        if signature != self.signature:
            self.signature, self.pending_since = signature, now
        if self.pending_since is None or now - self.pending_since < self.debounce: return None
        self.pending_since = None
        stats = {}
        try:
            report = build(self.manifest, self.output, self.content, self.aseprite, self.cache_dir, stats)
            self.last_error = None
            return {"ok": True, "files": len(report["files"]), "cache": stats}
        except (ImportError, OSError, json.JSONDecodeError) as error:
            self.last_error = str(error)
            return {"ok": False, "error": self.last_error}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--content", type=Path, help="copy source content beside generated assets")
    parser.add_argument("--aseprite", default="aseprite", help="Aseprite executable for native .ase/.aseprite files")
    parser.add_argument("--cache", type=Path, help="processed artifact cache (default: hidden directory in output)")
    parser.add_argument("--watch", action="store_true", help="reimport changed authoring sources while the game runs")
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--debounce", type=float, default=0.1)
    args = parser.parse_args()
    if args.watch:
        if not math.isfinite(args.interval) or args.interval <= 0 or not math.isfinite(args.debounce) or args.debounce < 0:
            parser.error("watch interval must be positive and debounce nonnegative")
        watcher = ImportWatcher(args.manifest, args.output, args.content, args.aseprite, args.cache, args.debounce)
        try:
            while True:
                result = watcher.poll()
                if result is not None:
                    print(json.dumps(result), file=sys.stdout if result["ok"] else sys.stderr, flush=True)
                time.sleep(args.interval)
        except KeyboardInterrupt:
            return 0
    try:
        stats = {}
        report = build(args.manifest, args.output, args.content, args.aseprite, args.cache, stats)
    except (ImportError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"ok": True, "files": len(report["files"]), "cache": stats}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
