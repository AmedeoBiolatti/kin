from __future__ import annotations
import copy
from pathlib import Path
from .common import (
    EvalError,
    executable_path,
    identifier,
    number,
    read_json,
    resolve,
    sha256,
)

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
STAGES = ("bench", "capture", "truth", "score", "stress")
DEFAULTS = {
    "smoke": dict(
        runs=1,
        warmup=4,
        frames=8,
        size=[320, 180],
        samples=16,
        seeds=[0, 1],
        stages=["bench", "capture", "truth", "score"],
        metrics=["psnr", "ssim", "bias", "detail", "temporal", "shimmer"],
    ),
    "standard": dict(
        runs=5,
        warmup=300,
        frames=1500,
        size=[1920, 1080],
        samples=1024,
        seeds=[0, 1],
        stages=["bench", "capture", "truth", "score"],
        metrics=[
            "psnr",
            "ssim",
            "bias",
            "detail",
            "temporal",
            "shimmer",
            "flip",
            "jod",
        ],
    ),
}


def load_suite(path, profile="smoke", scenes=None, baseline=None, candidate=None):
    path = Path(path).resolve()
    raw = read_json(path)
    if raw.get("schema") != "kin.eval-suite/1":
        raise EvalError("expected kin.eval-suite/1")
    unknown = set(raw) - {
        "schema",
        "scenes",
        "builds",
        "profiles",
        "tools",
        "display",
        "thresholds",
        "cache",
    }
    if unknown:
        raise EvalError(f"unknown suite keys: {sorted(unknown)}")
    if profile not in DEFAULTS and profile not in raw.get("profiles", {}):
        raise EvalError(f"unknown profile: {profile}")
    settings = copy.deepcopy(DEFAULTS.get(profile, DEFAULTS["standard"]))
    profile_keys = set(settings) | {
        "timeout",
        "max_score_pixels",
        "stress_cycles",
        "stress_hold_frames",
        "stress_settle_frames",
        "stress_tolerance_bytes",
    }
    if set(raw.get("profiles", {}).get(profile, {})) - profile_keys:
        raise EvalError("unknown profile settings")
    settings.update(raw.get("profiles", {}).get(profile, {}))
    for key, lo, hi in (
        ("runs", 1, 100),
        ("warmup", 0, 100000),
        ("frames", 1, 1000000),
        ("samples", 1, 65536),
    ):
        settings[key] = number(settings[key], key, lo, hi, True)
    if len(settings["size"]) != 2:
        raise EvalError("size needs width,height")
    settings["size"] = [number(v, "size", 1, 16384, True) for v in settings["size"]]
    if not settings.get("seeds") or len(set(settings["seeds"])) != len(
        settings["seeds"]
    ):
        raise EvalError("distinct reference seeds required")
    settings["seeds"] = [
        number(v, "seed", 0, 2**31 - 1, True) for v in settings["seeds"]
    ]
    if (
        not isinstance(settings["stages"], list)
        or not settings["stages"]
        or len(settings["stages"]) != len(set(settings["stages"]))
        or any(s not in STAGES for s in settings["stages"])
    ):
        raise EvalError("unknown stage")
    known_metrics = {
        "psnr",
        "ssim",
        "bias",
        "detail",
        "temporal",
        "shimmer",
        "flip",
        "jod",
    }
    if (
        not isinstance(settings.get("metrics"), list)
        or set(settings["metrics"]) - known_metrics
    ):
        raise EvalError("unknown metric provider")
    for key in (
        "timeout",
        "max_score_pixels",
        "stress_cycles",
        "stress_hold_frames",
        "stress_settle_frames",
    ):
        if key in settings:
            number(settings[key], key, 1, 1000000000, True)
    selected, seen = [], set()
    for value in raw.get("scenes", []):
        if isinstance(value, str):
            scene_path = resolve(value, path.parent)
            data = read_json(scene_path)
            base = scene_path.parent
        else:
            data = copy.deepcopy(value)
            scene_path = path
            base = path.parent
        sid = identifier(data.get("id"))
        if sid in seen:
            raise EvalError(f"duplicate scene: {sid}")
        seen.add(sid)
        if scenes and sid not in scenes:
            continue
        if data.get("schema") != "kin.eval-scene/1":
            raise EvalError(f"{sid}: expected kin.eval-scene/1")
        if data.get("adapter") not in ("reference", "yard", "bistro", "command"):
            raise EvalError(f"{sid}: unknown adapter")
        data["manifest"] = str(scene_path)
        for key in ("source", "asset_root", "project", "script"):
            if key in data:
                data[key] = str(resolve(data[key], base))
        for key in ("dependencies",):
            data[key] = [str(resolve(v, base)) for v in data.get(key, [])]
        data.setdefault("args", [])
        if not all(isinstance(a, str) for a in data["args"]):
            raise EvalError(f"{sid}: args must be strings")
        data["_base"] = str(base)
        selected.append(data)
    if scenes and set(scenes) - seen:
        raise EvalError(f"unknown scenes: {sorted(set(scenes) - seen)}")
    if not selected:
        raise EvalError("no scenes selected")
    builds = copy.deepcopy(raw.get("builds", {}))
    for label, override in (("baseline", baseline), ("candidate", candidate)):
        if override:
            builds[label] = str(Path(override).resolve())
    if not builds:
        raise EvalError("at least one build required")
    normalized = {}
    for label, value in builds.items():
        identifier(label)
        if isinstance(value, str):
            bp = resolve(value, path.parent)
            data = read_json(bp)
            base = bp.parent
            if data.get("schema") != "kin.eval-build/1":
                raise EvalError("expected kin.eval-build/1")
        else:
            data = value
            base = path.parent
        data = copy.deepcopy(data)
        data["executables"] = {
            k: str(executable_path(v, base))
            for k, v in data.get("executables", {}).items()
        }
        data["args"] = data.get("args", [])
        if "source_root" in data:
            data["source_root"] = str(resolve(data["source_root"], base))
        normalized[label] = data
    tools = raw.get("tools", {})
    tools = {k: str(executable_path(v, path.parent)) for k, v in tools.items()}
    display = raw.get("display", {})
    if display.get("file"):
        display = dict(display, file=str(resolve(display["file"], path.parent)))
    return {
        "schema": "kin.eval-plan/1",
        "suite": str(path),
        "profile_name": profile,
        "profile": settings,
        "scenes": selected,
        "builds": normalized,
        "tools": tools,
        "display": display,
        "thresholds": raw.get("thresholds", {"performance_review_percent": 5}),
        "cache": str(resolve(raw.get("cache", "~/.cache/kin/eval"), path.parent)),
    }


def input_files(scene):
    """Enumerate content, including external glTF buffer/image dependencies."""
    files, visited = {}, set()

    def add(path):
        p = Path(path).resolve()
        if p in visited:
            return
        visited.add(p)
        if p.is_dir():
            for child in sorted(p.rglob("*")):
                if child.is_file():
                    add(child)
            return
        if not p.is_file():
            raise EvalError(f"missing dependency: {p}")
        files[str(p)] = sha256(p)
        if p.suffix.lower() == ".gltf":
            from urllib.parse import unquote

            doc = read_json(p)
            for item in doc.get("buffers", []) + doc.get("images", []):
                uri = item.get("uri", "")
                if uri and not uri.startswith("data:"):
                    add(p.parent / unquote(uri))

    add(scene["manifest"])
    if scene.get("source"):
        add(scene["source"])
    for p in scene.get("dependencies", []):
        add(p)
    if scene["adapter"] == "reference":
        doc = read_json(scene["source"])

        def visit(value):
            if isinstance(value, dict):
                for key, v in value.items():
                    if key in ("file", "image", "environment") and isinstance(v, str):
                        add(Path(scene.get("asset_root", ".")) / v)
                    else:
                        visit(v)
            elif isinstance(value, list):
                for v in value:
                    visit(v)

        visit(doc)
    return files


def timeline(scene, count=None):
    """Resolve paths once; all renderers consume the same explicit frame cameras."""
    path = scene.get("timeline")
    if not path:
        return None
    fps = number(path.get("fps", 60), "timeline fps", 0.01, 1000)
    keys = path.get("keys", [])
    if not keys or keys[0].get("frame") != 0:
        raise EvalError("timeline must start at frame zero")
    last = -1
    for key in keys:
        f = number(key.get("frame"), "key frame", 0, 100000, True)
        if f <= last:
            raise EvalError("timeline keys must increase")
        last = f
        for name in ("eye", "target"):
            if len(key.get(name, [])) != 3:
                raise EvalError(f"key needs {name}")
            for v in key[name]:
                number(v, name)
        number(key.get("fov", 60), "fov", 0.1, 179)
    capture = path.get("capture", [last])
    if not capture or sorted(set(capture)) != capture:
        raise EvalError("capture IDs must be distinct and increasing")
    for f in capture:
        number(f, "capture frame", 0, last, True)
    if len(capture) > 1 and any(b - a != 1 for a, b in zip(capture, capture[1:])):
        raise EvalError(
            "video captures require contiguous frame IDs; use separate still cases for sparse samples"
        )
    frames = []
    for i in range((count or last + 1)):
        k = min(i, last)
        a = keys[0]
        b = a
        for candidate in keys[1:]:
            b = candidate
            if k <= b["frame"]:
                break
            a = b
        t = (
            0
            if a["frame"] == b["frame"]
            else (k - a["frame"]) / (b["frame"] - a["frame"])
        )
        row = {
            name: [x + (y - x) * t for x, y in zip(a[name], b[name])]
            for name in ("eye", "target")
        }
        row["fov"] = a.get("fov", 60) + (b.get("fov", 60) - a.get("fov", 60)) * t
        row["cut"] = i in path.get("cuts", [])
        if sum((x - y) ** 2 for x, y in zip(row["eye"], row["target"])) < 1e-16:
            raise EvalError("timeline eye and target coincide")
        frames.append(row)
    return {"fps": fps, "capture": capture, "frames": frames}
