"""Offline reference discovery matching Kin's built-in asset path semantics.

This inspects data; it never executes Lua or treats observed loads as roots.
"""
import json
from pathlib import PurePosixPath
import posixpath


LEAVES = {".png", ".jpg", ".jpeg", ".bmp", ".qoi", ".wav", ".ogg", ".flac",
          ".mp3", ".ttf", ".otf", ".spv", ".kinlang"}


def reference(owner, value, inventory, relative_to_catalog=False):
    if (not isinstance(value, str) or not value or value.startswith("/") or "\\" in value
            or ":" in value or any(ord(char) < 32 for char in value)):
        raise ValueError(f"Invalid dependency path in {owner}: {value!r}")
    base = str(PurePosixPath(owner).parent) if relative_to_catalog else "."

    def resolve(prefix):
        path = posixpath.normpath(posixpath.join(prefix, value))
        if path in (".", "..") or path.startswith("../"):
            raise ValueError(f"Dependency escapes asset root in {owner}: {value!r}")
        return path

    result = {resolve(base)}
    # Localization::localized_path checks l10n/<tag>/<relative>. Retain every
    # existing locale variant, without assuming one training/user locale.
    prefix = "" if base == "." else base + "/"
    localized = prefix + "l10n/"
    tags = {path[len(localized):].split("/", 1)[0] for path in inventory if path.startswith(localized)}
    if not value.startswith("l10n/") or relative_to_catalog:
        for tag in tags:
            candidate = resolve(localized + tag)
            if candidate in inventory:
                result.add(candidate)
    return sorted(result)


def discover(root, path, inventory, object_hook):
    suffix = PurePosixPath(path).suffix.lower()
    if suffix in LEAVES:
        return {"format": "leaf", "complete": True, "edges": [], "notes": []}
    if suffix not in (".kinscene", ".kinprefab", ".kinsprites", ".kinaudio"):
        return None
    edges, notes = [], []

    def add(value, location, catalog_relative=False):
        for target in reference(path, value, inventory, catalog_relative):
            edges.append({"source": path, "target": target, "origin": "scanner:" + suffix[1:], "location": location})

    text = (root / path).read_text(encoding="utf-8")
    if suffix in (".kinscene", ".kinprefab"):
        document = json.loads(text, object_pairs_hook=object_hook)
        expected = "kin.scene/1" if suffix == ".kinscene" else "kin.prefab/1"
        if not isinstance(document, dict) or document.get("schema", expected) != expected:
            raise ValueError(f"Unsupported {suffix} schema in {path}")
        if suffix == ".kinscene":
            script = document.get("script", "")
            if not isinstance(script, str):
                raise ValueError(f"Scene script must be a path string: {path}")
            if script:
                add(script, "/script")
            prefabs = document.get("prefabs", [])
            if not isinstance(prefabs, list):
                raise ValueError(f"Scene prefabs must be an array: {path}")
            for index, prefab in enumerate(prefabs):
                if not isinstance(prefab, dict):
                    raise ValueError(f"Invalid prefab reference: {path}")
                add(prefab.get("asset"), f"/prefabs/{index}/asset")
        # Component fields can be game-defined strings/asset IDs. Their binding
        # to files cannot be inferred from the scene/prefab container schema.
        notes.append("component bindings require an explicit completeness declaration")
        complete = False
    else:
        directives = ({"texture": 3, "sheet": 5, "sprite": 7, "sheet_sprite": 4}
                      if suffix == ".kinsprites" else {"bus": 3, "clip": 3, "cue": 4})
        for number, line in enumerate(text.splitlines(), 1):
            tokens = line.split("#", 1)[0].split()
            if not tokens:
                continue
            if tokens[0] not in directives or len(tokens) < directives[tokens[0]]:
                raise ValueError(f"Unknown/malformed catalog directive in {path}:{number}")
            if tokens[0] in ("texture", "clip"):
                add(tokens[2], f"line:{number}", suffix == ".kinaudio")
        complete = True
    return {"format": suffix[1:], "complete": complete, "edges": edges, "notes": notes}
