"""Capture versioned quality candidates with source/content identities and alternating run order.

This runs the existing native reference binary and optionally its Cycles comparison.
It does not select defaults or treat static shimmer captures as moving-camera evidence.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def identity(binary, scenes, cache):
    paths = git("ls-files", "--cached", "--others", "--exclude-standard", "-z", "engine", "cmake", "tools/reference").decode().split("\0")
    source = {p: digest(ROOT / p) for p in sorted(set(paths)) if p and (ROOT / p).is_file()}
    content = {}

    def record(path, scene=False):
        path = path.resolve()
        if not scene and not path.is_relative_to(cache.resolve()):
            raise ValueError(f"asset dependency escapes reference cache: {path}")
        if not path.is_file() or str(path) in content:
            return
        content[str(path)] = digest(path)
        if path.suffix == ".gltf":
            document = json.loads(path.read_text())
            for entry in document.get("buffers", []) + document.get("images", []):
                uri = entry.get("uri", "")
                if uri and not uri.startswith("data:"):
                    from urllib.parse import unquote
                    record(path.parent / unquote(uri))

    def find_files(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key in ("file", "image", "environment") and isinstance(child, str):
                    record(cache / child)
                find_files(child)
        elif isinstance(value, list):
            for child in value:
                find_files(child)

    for scene in scenes:
        record(scene, scene=True)
        find_files(json.loads(scene.read_text()))
    build = binary.parent.parent
    build_info = {}
    for filename in ("CMakeCache.txt", "compile_commands.json"):
        path = build / filename
        if path.is_file():
            build_info[filename + "_sha256"] = digest(path)
    commands = build / "compile_commands.json"
    if commands.is_file():
        build_info["renderer_compile_commands"] = [entry for entry in json.loads(commands.read_text())
            if entry["file"].endswith(("gpu_renderer3d.cpp", "gpu_gi3d.cpp", "model.cpp"))]
    cache_file = build / "CMakeCache.txt"
    if cache_file.is_file():
        wanted = ("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:",
                  "KIN_RENDERER:", "KIN_RENDERER3D_O3:")
        build_info["cmake_settings"] = [line for line in cache_file.read_text().splitlines() if line.startswith(wanted)]
    hardware = {}
    if shutil.which("nvidia-smi"):
        query = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total,temperature.gpu,power.draw",
                                "--format=csv,noheader,nounits"], capture_output=True, text=True, check=False)
        hardware["nvidia_smi"] = query.stdout.strip() if query.returncode == 0 else "unavailable"
    return {
        "build": build_info, "hardware_at_start": hardware,
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": git("rev-parse", "HEAD").decode().strip(),
        "status": git("status", "--short").decode(),
        "binary": str(binary), "binary_sha256": digest(binary),
        "source_sha256": source, "content_sha256": content,
        "platform": platform.platform(),
        "environment": {k: os.environ[k] for k in ("DISPLAY", "VK_ICD_FILENAMES", "KIN_RENDER_BACKEND") if k in os.environ},
        "note": "Logical memory excludes SDL cycle backings, GPU retirement and driver allocations. Static images alone cannot calibrate temporal defaults.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/kin/reference")
    parser.add_argument("--scenes", default="cornell,furnace,spheres,shadows,helmet,sponza")
    parser.add_argument("--candidates", type=Path, default=HERE / "quality_candidates.json")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=300)
    parser.add_argument("--frames", type=int, default=1500)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--truth", type=Path)
    parser.add_argument("extra", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.runs < 1 or args.frames < 1 or args.warmup < 0:
        parser.error("runs/frames must be positive; warmup must be nonnegative")
    args.binary = args.binary.resolve()
    args.out = args.out.resolve()
    args.cache = args.cache.resolve()
    scenes = [ROOT / "tests/reference/scenes" / f"{name}.json" for name in args.scenes.split(",")]
    for scene in scenes:
        if not scene.resolve().is_relative_to((ROOT / "tests/reference/scenes").resolve()) or not scene.is_file():
            parser.error(f"missing scene: {scene}")
    config = json.loads(args.candidates.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = identity(args.binary, scenes, args.cache)
    manifest.update({"candidates": config, "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}, "runs": []})
    destination = args.out / "manifest.json"
    if destination.exists():
        parser.error("output already contains a manifest; select a new run directory")
    destination.write_text(json.dumps(manifest, indent=2) + "\n")
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    for run in range(args.runs):
        candidates = list(config["candidates"].items())
        if run % 2:
            candidates.reverse()
        for name, options in candidates:
            out = args.out / f"run-{run:02}-{name}"
            command = [str(args.binary), *map(str, scenes), "--out", str(out), "--cache", str(args.cache),
                       "--warmup", str(args.warmup), "--frames", str(args.frames), "--repeat", "1",
                       "--width", str(args.width), "--height", str(args.height), *options, *extra]
            entry = {"run": run, "candidate": name, "command": command, "status": "running"}
            manifest["runs"].append(entry)
            destination.write_text(json.dumps(manifest, indent=2) + "\n")
            out.mkdir(parents=True)
            with (out / "capture.log").open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
            entry["returncode"] = result.returncode
            entry["status"] = "passed" if result.returncode == 0 and (out / "results.json").is_file() else "failed"
            destination.write_text(json.dumps(manifest, indent=2) + "\n")
            if entry["status"] != "passed":
                raise SystemExit(f"capture failed: {out / 'capture.log'}")
            if args.truth:
                subprocess.run([sys.executable, str(HERE / "compare.py"), str(out), str(args.truth),
                                "--out", str(out / "vs-truth")], check=True)
    print(destination)


if __name__ == "__main__":
    main()
